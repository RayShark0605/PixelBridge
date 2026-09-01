#include "metric_calibration_core.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbremotevisualmetriccalibration
{
namespace
{

inline constexpr double kCalibratedMagnitudeClip = 32767.0 / pbdesktoplevels::kSoftMetricScale;
inline constexpr double kProbabilityFloor = 1e-12;
inline constexpr double kFalseConfidenceMagnitude = 6.906754778648553;
inline constexpr std::array<double, 10> kReliabilityProbabilityUpper{
    0.0001, 0.001, 0.01, 0.025, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5};
inline constexpr std::array<double, 11> kConfidenceMagnitudeThresholds{
    0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2, kFalseConfidenceMagnitude,
    kCalibratedMagnitudeClip};
inline constexpr std::array<double, 23> kRocOneScoreThresholds{
    -kCalibratedMagnitudeClip - 1.0, -kCalibratedMagnitudeClip,
    -kFalseConfidenceMagnitude, -3.2, -1.6, -0.8, -0.4,
    -0.2, -0.1, -0.05, -0.02, 0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2,
    kFalseConfidenceMagnitude, kCalibratedMagnitudeClip, kCalibratedMagnitudeClip + 1.0};
inline constexpr std::array<double, 16> kPiecewiseUpper{
    0.01, 0.02, 0.04, 0.08, 0.12, 0.2, 0.35, 0.5,
    0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 8.0, 1000000.0};
inline constexpr std::array<double, 7> kGlobalScaleCandidates{0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};

struct PreparedTruth
{
    std::array<std::byte, pbmodulation::kRemoteVisualLowFpsDataBytes> codedData{};
    std::array<pbdesktoplevels::AcceptedTransportBlock, pbmodulation::kRemoteVisualLowFpsCodewords> blocks{};
};

struct FrameMetadata
{
    std::string datasetId;
    std::string runId;
    DatasetSplit split = DatasetSplit::Train;
    TruthAvailability truthAvailability = TruthAvailability::DiagnosticSenderFixture;
    std::string signalProfile;
    std::array<std::byte, pbmodulation::kLocalDesktopBootstrapRecordBytes> sourceBootstrap{};
};

struct PreparedInput
{
    std::map<std::string, const MetricObservation*> observations;
    std::map<std::string, FrameMetadata> frames;
    std::map<std::string, PreparedTruth> truth;
    std::map<std::string, std::string> policyManifests;
    std::map<std::string, bool> policyAdmissionRelaxation;
    std::vector<std::string> frameKeys;
    std::vector<std::string> policyIds;
    std::vector<DatasetGroupReport> datasetGroups;
    std::string inputBlake3;
};

[[nodiscard]] std::string MakeGroupKey(const MetricObservation& observation)
{
    return observation.datasetId + "/" + observation.runId;
}

[[nodiscard]] std::string MakeFrameKey(const MetricObservation& observation)
{
    return MakeGroupKey(observation) + "/" + observation.frameId;
}

[[nodiscard]] std::string MakeObservationKey(const MetricObservation& observation)
{
    return observation.policyId + "\x1f" + MakeFrameKey(observation);
}

[[nodiscard]] bool IsIdentifier(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > 128)
    {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character)
    {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.';
    });
}

[[nodiscard]] bool IsKnownSplit(const DatasetSplit split) noexcept
{
    return split == DatasetSplit::Train || split == DatasetSplit::Validation ||
        split == DatasetSplit::Holdout || split == DatasetSplit::External;
}

[[nodiscard]] bool IsKnownTruth(const TruthAvailability availability) noexcept
{
    return availability == TruthAvailability::DiagnosticSenderFixture ||
        availability == TruthAvailability::UnavailableReceiverOnly;
}

[[nodiscard]] bool EqualFrameMetadata(const FrameMetadata& metadata,
    const MetricObservation& observation) noexcept
{
    return metadata.split == observation.split && metadata.truthAvailability == observation.truthAvailability &&
        metadata.datasetId == observation.datasetId && metadata.runId == observation.runId &&
        metadata.signalProfile == observation.signalProfile && metadata.sourceBootstrap == observation.sourceBootstrap;
}

[[nodiscard]] std::string BytesToHex(std::span<const std::byte> bytes);

void UpdateUnsigned64(pbprotocol::Blake3Hasher& hasher, const std::uint64_t value)
{
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFU);
    }
    hasher.Update(bytes);
}

void UpdateString(pbprotocol::Blake3Hasher& hasher, const std::string_view value)
{
    UpdateUnsigned64(hasher, value.size());
    hasher.Update(std::as_bytes(std::span(value.data(), value.size())));
}

[[nodiscard]] std::string HashPreparedObservations(
    const std::map<std::string, const MetricObservation*>& observations)
{
    pbprotocol::Blake3Hasher hasher;
    static constexpr std::string_view domain = "PixelBridge.RemoteVisualMetricCalibration.Input.1";
    hasher.Update(std::as_bytes(std::span(domain.data(), domain.size())));
    UpdateUnsigned64(hasher, observations.size());
    for (const auto& [key, pointer] : observations)
    {
        const MetricObservation& observation = *pointer;
        UpdateString(hasher, key);
        UpdateString(hasher, observation.signalProfile);
        UpdateString(hasher, observation.policyManifestJson);
        const std::array<std::byte, 4> flags{
            static_cast<std::byte>(observation.split),
            static_cast<std::byte>(observation.truthAvailability),
            static_cast<std::byte>(observation.receiverBootstrapAvailable),
            static_cast<std::byte>(observation.modulationAccepted)};
        hasher.Update(flags);
        const std::array<std::byte, 1> policyFlags{
            static_cast<std::byte>(observation.policyRelaxesDefaultAdmission)};
        hasher.Update(policyFlags);
        hasher.Update(observation.sourceBootstrap);
        hasher.Update(observation.receiverBootstrap);
        UpdateString(hasher, observation.modulationErasure);
        UpdateUnsigned64(hasher, observation.rawMetrics.size());
        for (const float metric : observation.rawMetrics)
        {
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(metric);
            std::array<std::byte, 4> encoded{};
            for (std::size_t index = 0; index < encoded.size(); index++)
            {
                encoded[index] = static_cast<std::byte>((bits >> (index * 8)) & 0xFFU);
            }
            hasher.Update(encoded);
        }
    }
    return BytesToHex(hasher.Finalize());
}

[[nodiscard]] bool MakePreparedTruth(
    const std::array<std::byte, pbmodulation::kLocalDesktopBootstrapRecordBytes>& bootstrap,
    PreparedTruth& output, std::string& error)
{
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrap);
    if (!parsed || parsed.Value().visualProfileId != pbmodulation::kRemoteVisualLowFpsProfileId ||
        parsed.Value().visualLayoutVersion != pbmodulation::kRemoteVisualLowFpsLayoutVersion)
    {
        error = "sender-truth Bootstrap is not canonical LF4";
        return false;
    }
    PreparedTruth truth;
    if (!pbdesktoplevels::GenerateDiagnosticData(bootstrap, truth.codedData))
    {
        error = "diagnostic sender truth generation failed";
        return false;
    }
    for (std::uint32_t slot = 0; slot < truth.blocks.size(); slot++)
    {
        const auto information = std::span(truth.codedData).subspan(
            static_cast<std::size_t>(slot) * pbdesktoplevels::kCodewordBytes,
            pbdesktoplevels::kInfoBytes);
        const auto extracted = pbprotocol::ExtractTransportBlockFromInfoBlock(information);
        if (!extracted)
        {
            error = "sender-truth Transport extraction failed";
            return false;
        }
        auto& block = truth.blocks[slot];
        block.slot = slot;
        block.byteCount = static_cast<std::uint32_t>(extracted.Value().size());
        std::copy(extracted.Value().begin(), extracted.Value().end(), block.bytes.begin());
    }
    output = std::move(truth);
    return true;
}

[[nodiscard]] bool PrepareInput(const std::span<const MetricObservation> observations,
    PreparedInput& output, std::string& error)
{
    if (observations.empty() || observations.size() > 4096)
    {
        error = "calibration observations must contain 1..4096 bounded records";
        return false;
    }
    PreparedInput prepared;
    std::map<std::string, DatasetSplit> groupSplits;
    std::set<std::string> policyIds;
    std::set<std::string> frameKeys;
    for (const MetricObservation& observation : observations)
    {
        if (!IsIdentifier(observation.datasetId) || !IsIdentifier(observation.runId) ||
            !IsIdentifier(observation.frameId) || !IsIdentifier(observation.signalProfile) ||
            !IsIdentifier(observation.policyId) || observation.policyManifestJson.empty() ||
            observation.policyManifestJson.size() > 4096 || !IsKnownSplit(observation.split) ||
            !IsKnownTruth(observation.truthAvailability))
        {
            error = "calibration observation contains an invalid identifier, enum or policy manifest";
            return false;
        }
        if (observation.truthAvailability == TruthAvailability::UnavailableReceiverOnly &&
            observation.split != DatasetSplit::External)
        {
            error = "receiver-only truth is allowed only in the External split";
            return false;
        }
        if (observation.modulationAccepted != observation.receiverBootstrapAvailable ||
            (observation.modulationAccepted &&
                (observation.rawMetrics.size() != pbmodulation::kRemoteVisualLowFpsCodedBits ||
                    observation.rawMetrics.data() == nullptr || observation.modulationErasure != "None")) ||
            (!observation.modulationAccepted && !observation.rawMetrics.empty()) ||
            !std::ranges::all_of(observation.rawMetrics, [](const float value) { return std::isfinite(value); }))
        {
            error = "calibration observation modulation result is internally inconsistent";
            return false;
        }
        if (observation.modulationAccepted)
        {
            const auto receiver = pbprotocol::ParseBootstrapRecord(observation.receiverBootstrap);
            if (!receiver || receiver.Value().visualProfileId != pbmodulation::kRemoteVisualLowFpsProfileId ||
                receiver.Value().visualLayoutVersion != pbmodulation::kRemoteVisualLowFpsLayoutVersion)
            {
                error = "accepted calibration observation has a noncanonical receiver Bootstrap";
                return false;
            }
        }
        const std::string groupKey = MakeGroupKey(observation);
        const auto group = groupSplits.find(groupKey);
        if (group != groupSplits.end() && group->second != observation.split)
        {
            error = "dataset/run group leaks across calibration splits: " + groupKey;
            return false;
        }
        groupSplits[groupKey] = observation.split;
        const std::string frameKey = MakeFrameKey(observation);
        const auto frame = prepared.frames.find(frameKey);
        if (frame == prepared.frames.end())
        {
            FrameMetadata metadata;
            metadata.datasetId = observation.datasetId;
            metadata.runId = observation.runId;
            metadata.split = observation.split;
            metadata.truthAvailability = observation.truthAvailability;
            metadata.signalProfile = observation.signalProfile;
            metadata.sourceBootstrap = observation.sourceBootstrap;
            prepared.frames.emplace(frameKey, std::move(metadata));
            frameKeys.insert(frameKey);
            if (observation.truthAvailability == TruthAvailability::DiagnosticSenderFixture)
            {
                PreparedTruth truth;
                if (!MakePreparedTruth(observation.sourceBootstrap, truth, error))
                {
                    return false;
                }
                prepared.truth.emplace(frameKey, std::move(truth));
            }
        }
        else if (!EqualFrameMetadata(frame->second, observation))
        {
            error = "policy variants disagree on immutable frame metadata: " + frameKey;
            return false;
        }
        const auto manifest = prepared.policyManifests.find(observation.policyId);
        if (manifest != prepared.policyManifests.end() && manifest->second != observation.policyManifestJson)
        {
            error = "one policy id maps to conflicting manifests: " + observation.policyId;
            return false;
        }
        prepared.policyManifests[observation.policyId] = observation.policyManifestJson;
        const auto admission = prepared.policyAdmissionRelaxation.find(observation.policyId);
        if (admission != prepared.policyAdmissionRelaxation.end() &&
            admission->second != observation.policyRelaxesDefaultAdmission)
        {
            error = "one policy id has conflicting default-admission classification: " + observation.policyId;
            return false;
        }
        prepared.policyAdmissionRelaxation[observation.policyId] =
            observation.policyRelaxesDefaultAdmission;
        const std::string observationKey = MakeObservationKey(observation);
        if (!prepared.observations.emplace(observationKey, &observation).second)
        {
            error = "duplicate policy/frame calibration observation: " + observationKey;
            return false;
        }
        policyIds.insert(observation.policyId);
    }
    if (!policyIds.contains(kDefaultPolicyId))
    {
        error = "calibration input has no lf4-default policy baseline";
        return false;
    }
    for (const std::string& policyId : policyIds)
    {
        const bool missingFrame = std::ranges::any_of(frameKeys, [&](const std::string& frameKey)
        {
            return !prepared.observations.contains(policyId + "\x1f" + frameKey);
        });
        if (missingFrame)
        {
            error = "calibration policies do not cover an identical frame set";
            return false;
        }
    }
    std::array<std::uint32_t, 4> splitGroups{};
    for (const auto& [groupKey, split] : groupSplits)
    {
        static_cast<void>(groupKey);
        splitGroups[static_cast<std::size_t>(split)]++;
    }
    if (std::ranges::any_of(splitGroups, [](const std::uint32_t value) { return value == 0; }))
    {
        error = "train, validation, holdout and external must each contain at least one dataset/run group";
        return false;
    }
    prepared.frameKeys.assign(frameKeys.begin(), frameKeys.end());
    prepared.policyIds.assign(policyIds.begin(), policyIds.end());
    std::map<std::string, DatasetGroupReport> groupReports;
    for (const auto& [frameKey, metadata] : prepared.frames)
    {
        static_cast<void>(frameKey);
        const std::string groupKey = metadata.datasetId + "/" + metadata.runId;
        auto [group, inserted] = groupReports.try_emplace(groupKey);
        if (inserted)
        {
            group->second.datasetId = metadata.datasetId;
            group->second.runId = metadata.runId;
            group->second.signalProfile = metadata.signalProfile;
            group->second.split = metadata.split;
            group->second.truthAvailability = metadata.truthAvailability;
        }
        else if (group->second.signalProfile != metadata.signalProfile ||
            group->second.truthAvailability != metadata.truthAvailability)
        {
            error = "one dataset/run group has conflicting signal profile or truth availability";
            return false;
        }
        group->second.frames++;
    }
    for (auto& [groupKey, group] : groupReports)
    {
        static_cast<void>(groupKey);
        prepared.datasetGroups.push_back(std::move(group));
    }
    prepared.inputBlake3 = HashPreparedObservations(prepared.observations);
    output = std::move(prepared);
    return true;
}

[[nodiscard]] bool ExpectedBit(const PreparedTruth& truth, const std::size_t bit) noexcept
{
    return (std::to_integer<unsigned>(truth.codedData[bit / 8]) & (1u << (bit % 8))) != 0;
}

[[nodiscard]] bool MetricBit(const float metric) noexcept
{
    return metric < 0;
}

[[nodiscard]] double ErrorProbability(const double magnitude) noexcept
{
    const double bounded = std::clamp(magnitude, 0.0, 64.0);
    return 1.0 / (1.0 + std::exp(bounded));
}

[[nodiscard]] double SampleLogLoss(const double probability, const bool error) noexcept
{
    const double bounded = std::clamp(probability, kProbabilityFloor, 1.0 - kProbabilityFloor);
    return error ? -std::log(bounded) : -std::log1p(-bounded);
}

[[nodiscard]] std::size_t FindPiecewiseBin(const double magnitude) noexcept
{
    const auto found = std::ranges::lower_bound(kPiecewiseUpper, magnitude);
    return found == kPiecewiseUpper.end() ? kPiecewiseUpper.size() - 1 :
        static_cast<std::size_t>(std::distance(kPiecewiseUpper.begin(), found));
}

template<typename Visitor>
void VisitTrainingSamples(const PreparedInput& input, const std::string& policyId,
    const std::optional<std::string_view> signalProfile, Visitor&& visitor)
{
    for (const std::string& frameKey : input.frameKeys)
    {
        const auto& metadata = input.frames.at(frameKey);
        if (metadata.split != DatasetSplit::Train ||
            metadata.truthAvailability != TruthAvailability::DiagnosticSenderFixture ||
            (signalProfile && metadata.signalProfile != *signalProfile))
        {
            continue;
        }
        const MetricObservation& observation = *input.observations.at(policyId + "\x1f" + frameKey);
        if (!observation.modulationAccepted)
        {
            continue;
        }
        const PreparedTruth& truth = input.truth.at(frameKey);
        for (std::size_t bit = 0; bit < observation.rawMetrics.size(); bit++)
        {
            const float metric = observation.rawMetrics[bit];
            visitor(metric, MetricBit(metric) != ExpectedBit(truth, bit));
        }
    }
}

[[nodiscard]] double FitGlobalScale(const PreparedInput& input, const std::string& policyId,
    const std::optional<std::string_view> signalProfile)
{
    double selectedScale = 1;
    double selectedLoss = std::numeric_limits<double>::infinity();
    std::uint64_t selectedSamples = 0;
    for (const double scale : kGlobalScaleCandidates)
    {
        double loss = 0;
        std::uint64_t samples = 0;
        VisitTrainingSamples(input, policyId, signalProfile, [&](const float metric, const bool error)
        {
            const double magnitude = std::min(std::abs(static_cast<double>(metric)) * scale,
                kCalibratedMagnitudeClip);
            loss += SampleLogLoss(ErrorProbability(magnitude), error);
            samples++;
        });
        if (samples != 0 && (loss < selectedLoss || (loss == selectedLoss && scale < selectedScale)))
        {
            selectedLoss = loss;
            selectedScale = scale;
            selectedSamples = samples;
        }
    }
    return selectedSamples == 0 ? 1.0 : selectedScale;
}

[[nodiscard]] MetricModel FitGlobalModel(const PreparedInput& input, const std::string& policyId)
{
    MetricModel model;
    model.kind = MetricModelKind::GlobalScale;
    model.globalScale = FitGlobalScale(input, policyId, std::nullopt);
    return model;
}

[[nodiscard]] MetricModel FitPerSignalModel(const PreparedInput& input, const std::string& policyId)
{
    MetricModel model = FitGlobalModel(input, policyId);
    model.kind = MetricModelKind::PerSignalScale;
    std::set<std::string> signalProfiles;
    for (const auto& [frameKey, metadata] : input.frames)
    {
        static_cast<void>(frameKey);
        if (metadata.split == DatasetSplit::Train)
        {
            signalProfiles.insert(metadata.signalProfile);
        }
    }
    for (const std::string& signalProfile : signalProfiles)
    {
        model.signalScales.push_back({signalProfile,
            FitGlobalScale(input, policyId, std::optional<std::string_view>{signalProfile})});
    }
    return model;
}

[[nodiscard]] MetricModel FitPiecewiseModel(const PreparedInput& input, const std::string& policyId)
{
    struct IsotonicBlock
    {
        std::size_t first = 0;
        std::size_t last = 0;
        double errors = 0;
        double samples = 0;
    };
    std::array<std::uint64_t, kPiecewiseUpper.size()> samples{};
    std::array<std::uint64_t, kPiecewiseUpper.size()> errors{};
    VisitTrainingSamples(input, policyId, std::nullopt, [&](const float metric, const bool error)
    {
        const std::size_t bin = FindPiecewiseBin(std::abs(static_cast<double>(metric)));
        samples[bin]++;
        errors[bin] += error ? 1ULL : 0ULL;
    });
    const double fallbackScale = FitGlobalScale(input, policyId, std::nullopt);
    std::vector<IsotonicBlock> blocks;
    blocks.reserve(kPiecewiseUpper.size());
    double lower = 0;
    for (std::size_t bin = 0; bin < kPiecewiseUpper.size(); bin++)
    {
        IsotonicBlock block;
        block.first = bin;
        block.last = bin;
        if (samples[bin] == 0)
        {
            const double representative = (lower + kPiecewiseUpper[bin]) * 0.5;
            block.samples = 1;
            block.errors = ErrorProbability(std::min(representative * fallbackScale,
                kCalibratedMagnitudeClip));
        }
        else
        {
            block.samples = static_cast<double>(samples[bin]) + 1.0;
            block.errors = static_cast<double>(errors[bin]) + 0.5;
        }
        blocks.push_back(block);
        while (blocks.size() >= 2)
        {
            const IsotonicBlock& previous = blocks[blocks.size() - 2];
            const IsotonicBlock& current = blocks.back();
            const double previousProbability = previous.errors / previous.samples;
            const double currentProbability = current.errors / current.samples;
            if (previousProbability >= currentProbability)
            {
                break;
            }
            IsotonicBlock merged;
            merged.first = previous.first;
            merged.last = current.last;
            merged.errors = previous.errors + current.errors;
            merged.samples = previous.samples + current.samples;
            blocks.pop_back();
            blocks.back() = merged;
        }
        lower = kPiecewiseUpper[bin];
    }
    std::array<double, kPiecewiseUpper.size()> probabilities{};
    for (const IsotonicBlock& block : blocks)
    {
        const double probability = std::clamp(block.errors / block.samples, kProbabilityFloor, 0.5);
        for (std::size_t bin = block.first; bin <= block.last; bin++)
        {
            probabilities[bin] = probability;
        }
    }
    MetricModel model;
    model.kind = MetricModelKind::PiecewiseLookup;
    for (std::size_t bin = 0; bin < kPiecewiseUpper.size(); bin++)
    {
        const double magnitude = std::min(std::log((1.0 - probabilities[bin]) / probabilities[bin]),
            kCalibratedMagnitudeClip);
        model.piecewise.push_back({kPiecewiseUpper[bin], magnitude, samples[bin], errors[bin]});
    }
    return model;
}

[[nodiscard]] double FindSignalScale(const MetricModel& model, const std::string_view signalProfile) noexcept
{
    const auto found = std::ranges::find_if(model.signalScales, [&](const ScaleBinding& binding)
    {
        return binding.signalProfile == signalProfile;
    });
    return found == model.signalScales.end() ? model.globalScale : found->scale;
}

[[nodiscard]] float ApplyMetric(const MetricModel& model, const std::string_view signalProfile,
    const float metric)
{
    if (!std::isfinite(metric))
    {
        throw std::runtime_error("non-finite raw metric reached a calibration model");
    }
    if (model.kind == MetricModelKind::Raw || metric == 0)
    {
        return metric;
    }
    const double raw = static_cast<double>(metric);
    const double sign = raw < 0 ? -1.0 : 1.0;
    double magnitude = 0;
    if (model.kind == MetricModelKind::GlobalScale)
    {
        magnitude = std::abs(raw) * model.globalScale;
    }
    else if (model.kind == MetricModelKind::PerSignalScale)
    {
        magnitude = std::abs(raw) * FindSignalScale(model, signalProfile);
    }
    else
    {
        const double rawMagnitude = std::abs(raw);
        const auto found = std::ranges::find_if(model.piecewise, [&](const PiecewiseBinding& binding)
        {
            return rawMagnitude <= binding.rawMagnitudeUpper;
        });
        if (found == model.piecewise.end())
        {
            throw std::runtime_error("piecewise calibration has no terminal bin");
        }
        magnitude = found->calibratedMagnitude;
    }
    return static_cast<float>(sign * std::min(magnitude, kCalibratedMagnitudeClip));
}

[[nodiscard]] std::size_t FindReliabilityBin(const double probability) noexcept
{
    const auto found = std::ranges::lower_bound(kReliabilityProbabilityUpper, probability);
    return found == kReliabilityProbabilityUpper.end() ? kReliabilityProbabilityUpper.size() - 1 :
        static_cast<std::size_t>(std::distance(kReliabilityProbabilityUpper.begin(), found));
}

[[nodiscard]] bool SameAcceptedBlock(const pbdesktoplevels::AcceptedTransportBlock& accepted,
    const pbdesktoplevels::AcceptedTransportBlock& expected) noexcept
{
    return accepted.slot == expected.slot && accepted.byteCount == expected.byteCount &&
        accepted.byteCount <= accepted.bytes.size() &&
        std::equal(accepted.bytes.begin(), accepted.bytes.begin() + accepted.byteCount,
            expected.bytes.begin(), expected.bytes.begin() + expected.byteCount);
}

[[nodiscard]] EvaluationSummary Evaluate(const PreparedInput& input, const std::string& policyId,
    const MetricModel& model, const DatasetSplit split)
{
    EvaluationSummary summary;
    summary.reliability.reserve(kReliabilityProbabilityUpper.size());
    for (const double upper : kReliabilityProbabilityUpper)
    {
        summary.reliability.push_back({upper, 0, 0, 0});
    }
    summary.confidenceCurve.reserve(kConfidenceMagnitudeThresholds.size());
    for (const double threshold : kConfidenceMagnitudeThresholds)
    {
        summary.confidenceCurve.push_back({threshold, 0, 0});
    }
    summary.rocCurve.reserve(kRocOneScoreThresholds.size());
    for (const double threshold : kRocOneScoreThresholds)
    {
        summary.rocCurve.push_back({threshold, 0, 0});
    }
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(
        pbdesktoplevels::kProcessingReservationBytes);
    if (!channelResult)
    {
        throw std::runtime_error("calibration evaluation ReferenceChannel allocation failed");
    }
    auto channel = std::move(channelResult).Value();
    std::vector<float> calibrated(pbmodulation::kRemoteVisualLowFpsCodedBits);
    std::array<std::byte, pbmodulation::kRemoteVisualLowFpsDataBytes> hard{};
    for (const std::string& frameKey : input.frameKeys)
    {
        const FrameMetadata& metadata = input.frames.at(frameKey);
        if (metadata.split != split)
        {
            continue;
        }
        const MetricObservation& observation = *input.observations.at(policyId + "\x1f" + frameKey);
        summary.frames++;
        if (metadata.truthAvailability == TruthAvailability::DiagnosticSenderFixture)
        {
            summary.truthFrames++;
            summary.expectedTransportBlocks += pbmodulation::kRemoteVisualLowFpsCodewords;
        }
        else
        {
            summary.receiverOnlyFrames++;
        }
        if (!observation.modulationAccepted)
        {
            summary.modulationErasures++;
            continue;
        }
        hard.fill(std::byte{0});
        for (std::size_t bit = 0; bit < observation.rawMetrics.size(); bit++)
        {
            const float metric = ApplyMetric(model, metadata.signalProfile, observation.rawMetrics[bit]);
            calibrated[bit] = metric;
            if (metric < 0)
            {
                hard[bit / 8] |= static_cast<std::byte>(1u << (bit % 8));
            }
        }
        const auto evaluation = channel.EvaluateCodewords(observation.receiverBootstrap, hard,
            calibrated, pbdesktoplevels::EvaluationMode::Transport);
        summary.fecFailures += evaluation.fecFailures;
        summary.crcFailures += evaluation.crcFailures;
        summary.identityFailures += evaluation.identityFailures;
        summary.acceptedTransportBlocks += evaluation.acceptedTransportBlocks;
        summary.acceptedControlBlocks += evaluation.acceptedRemoteControlBlocks;
        summary.iterationsTotal += evaluation.iterationsTotal;
        summary.iterationsMaximum = std::max(summary.iterationsMaximum, evaluation.iterationsMaximum);
        const auto accepted = channel.GetAcceptedTransportBlocks();
        if (accepted.size() != evaluation.acceptedTransportBlocks)
        {
            throw std::runtime_error("calibration evaluation Transport snapshot count disagrees with its disposition");
        }
        std::uint32_t exactBlocks = 0;
        if (metadata.truthAvailability == TruthAvailability::DiagnosticSenderFixture)
        {
            const PreparedTruth& truth = input.truth.at(frameKey);
            for (const auto& block : accepted)
            {
                if (block.slot < truth.blocks.size() && SameAcceptedBlock(block, truth.blocks[block.slot]))
                {
                    exactBlocks++;
                }
                else
                {
                    summary.falseAcceptedTransportBlocks++;
                }
            }
            summary.falseAcceptedControlBlocks += evaluation.acceptedRemoteControlBlocks;
            for (std::size_t bit = 0; bit < calibrated.size(); bit++)
            {
                const bool expected = ExpectedBit(truth, bit);
                const bool actual = MetricBit(calibrated[bit]);
                const bool bitError = actual != expected;
                const double magnitude = std::abs(static_cast<double>(calibrated[bit]));
                const double probability = ErrorProbability(magnitude);
                summary.comparedBits++;
                summary.bitErrors += bitError ? 1ULL : 0ULL;
                summary.expectedOneBits += expected ? 1ULL : 0ULL;
                summary.expectedZeroBits += expected ? 0ULL : 1ULL;
                summary.oneBitErrors += expected && bitError ? 1ULL : 0ULL;
                summary.zeroBitErrors += !expected && bitError ? 1ULL : 0ULL;
                summary.highConfidenceBits += magnitude >= kFalseConfidenceMagnitude ? 1ULL : 0ULL;
                summary.falseConfidenceErrors += magnitude >= kFalseConfidenceMagnitude && bitError ? 1ULL : 0ULL;
                summary.logLossSum += SampleLogLoss(probability, bitError);
                const double errorValue = bitError ? 1.0 : 0.0;
                const double brierDifference = probability - errorValue;
                summary.brierScoreSum += brierDifference * brierDifference;
                ReliabilityBin& reliability = summary.reliability[FindReliabilityBin(probability)];
                reliability.samples++;
                reliability.errors += bitError ? 1ULL : 0ULL;
                reliability.predictedErrorSum += probability;
                for (ConfidencePoint& point : summary.confidenceCurve)
                {
                    if (magnitude >= point.minimumMagnitude)
                    {
                        point.samples++;
                        point.errors += bitError ? 1ULL : 0ULL;
                    }
                }
                const double oneScore = -static_cast<double>(calibrated[bit]);
                for (RocPoint& point : summary.rocCurve)
                {
                    if (oneScore >= point.minimumOneScore)
                    {
                        point.truePositive += expected ? 1ULL : 0ULL;
                        point.falsePositive += expected ? 0ULL : 1ULL;
                    }
                }
            }
            const bool verified = evaluation.evaluated && evaluation.paddingValid &&
                evaluation.codewords == pbmodulation::kRemoteVisualLowFpsCodewords &&
                exactBlocks == pbmodulation::kRemoteVisualLowFpsCodewords &&
                evaluation.acceptedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords &&
                evaluation.acceptedRemoteControlBlocks == 0 && evaluation.fecFailures == 0 &&
                evaluation.crcFailures == 0 && evaluation.identityFailures == 0;
            summary.verifiedFrames += verified ? 1U : 0U;
        }
    }
    if (summary.comparedBits != 0)
    {
        double expectedCalibrationError = 0;
        for (const ReliabilityBin& bin : summary.reliability)
        {
            if (bin.samples == 0)
            {
                continue;
            }
            const double observed = static_cast<double>(bin.errors) / static_cast<double>(bin.samples);
            const double predicted = bin.predictedErrorSum / static_cast<double>(bin.samples);
            expectedCalibrationError += std::abs(observed - predicted) *
                static_cast<double>(bin.samples) / static_cast<double>(summary.comparedBits);
        }
        summary.expectedCalibrationError = expectedCalibrationError;
    }
    return summary;
}

[[nodiscard]] double AverageLogLoss(const EvaluationSummary& summary) noexcept
{
    return summary.comparedBits == 0 ? std::numeric_limits<double>::infinity() :
        summary.logLossSum / static_cast<double>(summary.comparedBits);
}

[[nodiscard]] double BitErrorRate(const EvaluationSummary& summary) noexcept
{
    return summary.comparedBits == 0 ? std::numeric_limits<double>::infinity() :
        static_cast<double>(summary.bitErrors) / static_cast<double>(summary.comparedBits);
}

[[nodiscard]] double AverageBrierScore(const EvaluationSummary& summary) noexcept
{
    return summary.comparedBits == 0 ? std::numeric_limits<double>::infinity() :
        summary.brierScoreSum / static_cast<double>(summary.comparedBits);
}

[[nodiscard]] std::uint64_t ExactAcceptedBlocks(const EvaluationSummary& summary) noexcept
{
    return summary.acceptedTransportBlocks >= summary.falseAcceptedTransportBlocks ?
        summary.acceptedTransportBlocks - summary.falseAcceptedTransportBlocks : 0;
}

[[nodiscard]] bool BetterOnValidation(const CandidateReport& candidate,
    const CandidateReport& selected) noexcept
{
    const EvaluationSummary& first = candidate.validation;
    const EvaluationSummary& second = selected.validation;
    if (!first.IsAcceptanceSafe() || first.truthFrames == 0)
    {
        return false;
    }
    if (!second.IsAcceptanceSafe())
    {
        return true;
    }
    if (first.verifiedFrames != second.verifiedFrames)
    {
        return first.verifiedFrames > second.verifiedFrames;
    }
    if (ExactAcceptedBlocks(first) != ExactAcceptedBlocks(second))
    {
        return ExactAcceptedBlocks(first) > ExactAcceptedBlocks(second);
    }
    if (first.modulationErasures != second.modulationErasures)
    {
        return first.modulationErasures < second.modulationErasures;
    }
    if (first.falseConfidenceErrors != second.falseConfidenceErrors)
    {
        return first.falseConfidenceErrors < second.falseConfidenceErrors;
    }
    const double firstLoss = AverageLogLoss(first);
    const double secondLoss = AverageLogLoss(second);
    if (std::abs(firstLoss - secondLoss) > 1e-15)
    {
        return firstLoss < secondLoss;
    }
    if (std::abs(first.expectedCalibrationError - second.expectedCalibrationError) > 1e-15)
    {
        return first.expectedCalibrationError < second.expectedCalibrationError;
    }
    return first.iterationsTotal < second.iterationsTotal;
}

[[nodiscard]] bool ImprovedWithoutRegression(const EvaluationSummary& candidate,
    const EvaluationSummary& baseline, const bool requireAllTruth) noexcept
{
    if (!candidate.IsAcceptanceSafe() || candidate.truthFrames == 0 ||
        (requireAllTruth && candidate.receiverOnlyFrames != 0) ||
        candidate.frames != baseline.frames || candidate.truthFrames != baseline.truthFrames ||
        candidate.receiverOnlyFrames != baseline.receiverOnlyFrames ||
        candidate.expectedTransportBlocks != baseline.expectedTransportBlocks ||
        candidate.comparedBits < baseline.comparedBits ||
        candidate.verifiedFrames < baseline.verifiedFrames ||
        ExactAcceptedBlocks(candidate) < ExactAcceptedBlocks(baseline) ||
        candidate.modulationErasures > baseline.modulationErasures ||
        candidate.falseConfidenceErrors > baseline.falseConfidenceErrors ||
        candidate.fecFailures > baseline.fecFailures ||
        candidate.crcFailures > baseline.crcFailures ||
        candidate.identityFailures > baseline.identityFailures ||
        candidate.iterationsTotal > baseline.iterationsTotal ||
        candidate.iterationsMaximum > baseline.iterationsMaximum ||
        BitErrorRate(candidate) > BitErrorRate(baseline) + 1e-15 ||
        AverageLogLoss(candidate) > AverageLogLoss(baseline) + 1e-15 ||
        AverageBrierScore(candidate) > AverageBrierScore(baseline) + 1e-15 ||
        candidate.expectedCalibrationError > baseline.expectedCalibrationError + 1e-15)
    {
        return false;
    }
    return candidate.verifiedFrames > baseline.verifiedFrames ||
        ExactAcceptedBlocks(candidate) > ExactAcceptedBlocks(baseline) ||
        candidate.modulationErasures < baseline.modulationErasures ||
        candidate.falseConfidenceErrors < baseline.falseConfidenceErrors ||
        BitErrorRate(candidate) + 1e-15 < BitErrorRate(baseline) ||
        AverageLogLoss(candidate) + 1e-15 < AverageLogLoss(baseline) ||
        AverageBrierScore(candidate) + 1e-15 < AverageBrierScore(baseline) ||
        candidate.expectedCalibrationError + 1e-15 < baseline.expectedCalibrationError ||
        candidate.iterationsTotal < baseline.iterationsTotal;
}

[[nodiscard]] std::string ModelSuffix(const MetricModelKind kind)
{
    return GetMetricModelKindName(kind);
}

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("calibration integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendDouble(std::string& output, const double value)
{
    if (!std::isfinite(value))
    {
        output.append("null");
        return;
    }
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("calibration floating-point serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        }
        else if (character < 0x20)
        {
            output.append("\\u00");
            output.push_back(hexDigits[character >> 4]);
            output.push_back(hexDigits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string BytesToHex(const std::span<const std::byte> bytes)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned value = std::to_integer<unsigned>(bytes[index]);
        output[index * 2] = hexDigits[value >> 4];
        output[index * 2 + 1] = hexDigits[value & 0x0F];
    }
    return output;
}

void AppendRatio(std::string& output, const std::uint64_t numerator, const std::uint64_t denominator)
{
    AppendDouble(output, denominator == 0 ? std::numeric_limits<double>::infinity() :
        static_cast<double>(numerator) / static_cast<double>(denominator));
}

void AppendEvaluation(std::string& output, const EvaluationSummary& summary)
{
    output.append("{\"frames\":");
    AppendUnsigned(output, summary.frames);
    output.append(",\"truthFrames\":");
    AppendUnsigned(output, summary.truthFrames);
    output.append(",\"receiverOnlyFrames\":");
    AppendUnsigned(output, summary.receiverOnlyFrames);
    output.append(",\"modulationErasures\":");
    AppendUnsigned(output, summary.modulationErasures);
    output.append(",\"verifiedFrames\":");
    AppendUnsigned(output, summary.verifiedFrames);
    output.append(",\"frameErrorRate\":");
    AppendRatio(output, summary.truthFrames - summary.verifiedFrames, summary.truthFrames);
    output.append(",\"comparedBits\":");
    AppendUnsigned(output, summary.comparedBits);
    output.append(",\"bitErrors\":");
    AppendUnsigned(output, summary.bitErrors);
    output.append(",\"bitErrorRate\":");
    AppendRatio(output, summary.bitErrors, summary.comparedBits);
    output.append(",\"expectedZeroBits\":");
    AppendUnsigned(output, summary.expectedZeroBits);
    output.append(",\"expectedOneBits\":");
    AppendUnsigned(output, summary.expectedOneBits);
    output.append(",\"zeroBitErrors\":");
    AppendUnsigned(output, summary.zeroBitErrors);
    output.append(",\"oneBitErrors\":");
    AppendUnsigned(output, summary.oneBitErrors);
    output.append(",\"highConfidenceBits\":");
    AppendUnsigned(output, summary.highConfidenceBits);
    output.append(",\"falseConfidenceErrors\":");
    AppendUnsigned(output, summary.falseConfidenceErrors);
    output.append(",\"expectedTransportBlocks\":");
    AppendUnsigned(output, summary.expectedTransportBlocks);
    output.append(",\"acceptedTransportBlocks\":");
    AppendUnsigned(output, summary.acceptedTransportBlocks);
    output.append(",\"falseAcceptedTransportBlocks\":");
    AppendUnsigned(output, summary.falseAcceptedTransportBlocks);
    output.append(",\"acceptedControlBlocks\":");
    AppendUnsigned(output, summary.acceptedControlBlocks);
    output.append(",\"falseAcceptedControlBlocks\":");
    AppendUnsigned(output, summary.falseAcceptedControlBlocks);
    output.append(",\"fecFailures\":");
    AppendUnsigned(output, summary.fecFailures);
    output.append(",\"crcFailures\":");
    AppendUnsigned(output, summary.crcFailures);
    output.append(",\"identityFailures\":");
    AppendUnsigned(output, summary.identityFailures);
    output.append(",\"iterationsTotal\":");
    AppendUnsigned(output, summary.iterationsTotal);
    output.append(",\"iterationsMaximum\":");
    AppendUnsigned(output, summary.iterationsMaximum);
    output.append(",\"averageLogLoss\":");
    AppendDouble(output, AverageLogLoss(summary));
    output.append(",\"averageBrierScore\":");
    AppendDouble(output, summary.comparedBits == 0 ? std::numeric_limits<double>::infinity() :
        summary.brierScoreSum / static_cast<double>(summary.comparedBits));
    output.append(",\"expectedCalibrationError\":");
    AppendDouble(output, summary.comparedBits == 0 ? std::numeric_limits<double>::infinity() :
        summary.expectedCalibrationError);
    output.append(",\"reliability\":[");
    for (std::size_t index = 0; index < summary.reliability.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const ReliabilityBin& bin = summary.reliability[index];
        output.append("{\"probabilityUpper\":");
        AppendDouble(output, bin.probabilityUpper);
        output.append(",\"samples\":");
        AppendUnsigned(output, bin.samples);
        output.append(",\"errors\":");
        AppendUnsigned(output, bin.errors);
        output.append(",\"predictedErrorAverage\":");
        AppendDouble(output, bin.samples == 0 ? std::numeric_limits<double>::infinity() :
            bin.predictedErrorSum / static_cast<double>(bin.samples));
        output.append(",\"observedErrorRate\":");
        AppendRatio(output, bin.errors, bin.samples);
        output.push_back('}');
    }
    output.append("],\"confidenceCurve\":[");
    for (std::size_t index = 0; index < summary.confidenceCurve.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const ConfidencePoint& point = summary.confidenceCurve[index];
        output.append("{\"minimumMagnitude\":");
        AppendDouble(output, point.minimumMagnitude);
        output.append(",\"samples\":");
        AppendUnsigned(output, point.samples);
        output.append(",\"errors\":");
        AppendUnsigned(output, point.errors);
        output.append(",\"errorRate\":");
        AppendRatio(output, point.errors, point.samples);
        output.push_back('}');
    }
    output.append("],\"rocCurve\":[");
    for (std::size_t index = 0; index < summary.rocCurve.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const RocPoint& point = summary.rocCurve[index];
        output.append("{\"minimumOneScore\":");
        AppendDouble(output, point.minimumOneScore);
        output.append(",\"truePositive\":");
        AppendUnsigned(output, point.truePositive);
        output.append(",\"falsePositive\":");
        AppendUnsigned(output, point.falsePositive);
        output.append(",\"truePositiveRate\":");
        AppendRatio(output, point.truePositive, summary.expectedOneBits);
        output.append(",\"falsePositiveRate\":");
        AppendRatio(output, point.falsePositive, summary.expectedZeroBits);
        output.push_back('}');
    }
    output.append("]}");
}

void AppendModel(std::string& output, const MetricModel& model)
{
    output.append("{\"kind\":");
    AppendJsonString(output, GetMetricModelKindName(model.kind));
    output.append(",\"globalScale\":");
    AppendDouble(output, model.globalScale);
    output.append(",\"signalScales\":[");
    for (std::size_t index = 0; index < model.signalScales.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        output.append("{\"signalProfile\":");
        AppendJsonString(output, model.signalScales[index].signalProfile);
        output.append(",\"scale\":");
        AppendDouble(output, model.signalScales[index].scale);
        output.push_back('}');
    }
    output.append("],\"piecewise\":[");
    for (std::size_t index = 0; index < model.piecewise.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const PiecewiseBinding& binding = model.piecewise[index];
        output.append("{\"rawMagnitudeUpper\":");
        AppendDouble(output, binding.rawMagnitudeUpper);
        output.append(",\"calibratedMagnitude\":");
        AppendDouble(output, binding.calibratedMagnitude);
        output.append(",\"trainingSamples\":");
        AppendUnsigned(output, binding.trainingSamples);
        output.append(",\"trainingErrors\":");
        AppendUnsigned(output, binding.trainingErrors);
        output.push_back('}');
    }
    output.append("]}");
}

[[nodiscard]] std::string SerializePayload(const CalibrationReport& report)
{
    std::string output;
    output.reserve(32768 + report.candidates.size() * 32768);
    output.append("{\"input\":{\"observations\":");
    AppendUnsigned(output, report.inputObservations);
    output.append(",\"frames\":");
    AppendUnsigned(output, report.inputFrames);
    output.append(",\"blake3\":");
    AppendJsonString(output, report.inputBlake3);
    output.append(",\"datasetGroups\":[");
    for (std::size_t index = 0; index < report.datasetGroups.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const DatasetGroupReport& group = report.datasetGroups[index];
        output.append("{\"datasetId\":");
        AppendJsonString(output, group.datasetId);
        output.append(",\"runId\":");
        AppendJsonString(output, group.runId);
        output.append(",\"signalProfile\":");
        AppendJsonString(output, group.signalProfile);
        output.append(",\"split\":");
        AppendJsonString(output, GetDatasetSplitName(group.split));
        output.append(",\"truthAvailability\":");
        AppendJsonString(output, GetTruthAvailabilityName(group.truthAvailability));
        output.append(",\"frames\":");
        AppendUnsigned(output, group.frames);
        output.push_back('}');
    }
    output.append("],\"splitUnit\":\"datasetId/runId\"},\"truthBoundary\":{"
        "\"senderTruthEntersDemodulation\":false,"
        "\"senderTruthEntersFec\":false,\"postAdmissionTruthScoring\":true,"
        "\"receiverOnlyFalseAcceptance\":\"Unavailable\","
        "\"acceptanceAuthority\":\"QC-LDPC+padding+TransportCRC+SessionIdentity\","
        "\"wholeFileOutputEvaluated\":false,\"acceptedOutputObjects\":0,"
        "\"falseAcceptedOutputObjects\":0},\"selection\":{\"baselineCandidateId\":");
    AppendJsonString(output, report.baselineCandidateId);
    output.append(",\"selectedCandidateId\":");
    AppendJsonString(output, report.selectedCandidateId);
    output.append(",\"splitIsolationValid\":");
    AppendBoolean(output, report.splitIsolationValid);
    output.append(",\"selectionUsedHoldout\":");
    AppendBoolean(output, report.selectionUsedHoldout);
    output.append(",\"acceptanceAuthorityChanged\":");
    AppendBoolean(output, report.acceptanceAuthorityChanged);
    output.append(",\"productionDefaultsChanged\":");
    AppendBoolean(output, report.productionDefaultsChanged);
    output.append(",\"holdoutImproved\":");
    AppendBoolean(output, report.holdoutImproved);
    output.append(",\"externalImproved\":");
    AppendBoolean(output, report.externalImproved);
    output.append(",\"gatePassed\":");
    AppendBoolean(output, report.gatePassed);
    output.append("},\"candidates\":[");
    for (std::size_t index = 0; index < report.candidates.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const CandidateReport& candidate = report.candidates[index];
        output.append("{\"candidateId\":");
        AppendJsonString(output, candidate.candidateId);
        output.append(",\"policyId\":");
        AppendJsonString(output, candidate.policyId);
        output.append(",\"policyManifestJson\":");
        AppendJsonString(output, candidate.policyManifestJson);
        output.append(",\"deployableWithoutSignalProfileBinding\":");
        AppendBoolean(output, candidate.deployableWithoutSignalProfileBinding);
        output.append(",\"policyRelaxesDefaultAdmission\":");
        AppendBoolean(output, candidate.policyRelaxesDefaultAdmission);
        output.append(",\"model\":");
        AppendModel(output, candidate.model);
        output.append(",\"train\":");
        AppendEvaluation(output, candidate.train);
        output.append(",\"validation\":");
        AppendEvaluation(output, candidate.validation);
        output.append(",\"holdout\":");
        AppendEvaluation(output, candidate.holdout);
        output.append(",\"external\":");
        AppendEvaluation(output, candidate.external);
        output.push_back('}');
    }
    output.append("]}");
    return output;
}

[[nodiscard]] std::string SealPayload(const std::string& payload)
{
    const auto digest = pbprotocol::ComputeBlake3Digest(std::as_bytes(std::span(payload.data(), payload.size())));
    std::string output;
    output.reserve(payload.size() + 192);
    output.append("{\"schema\":");
    AppendJsonString(output, kMetricCalibrationSchema);
    output.append(",\"version\":");
    AppendUnsigned(output, kMetricCalibrationVersion);
    output.append(",\"payloadBlake3\":");
    AppendJsonString(output, BytesToHex(digest));
    output.append(",\"payload\":");
    output.append(payload);
    output.append("}\n");
    return output;
}

} // namespace

bool BuildCalibrationReport(const std::span<const MetricObservation> observations,
    CalibrationReport& output, std::string& error) noexcept
{
    try
    {
        PreparedInput input;
        if (!PrepareInput(observations, input, error))
        {
            return false;
        }
        CalibrationReport report;
        report.inputObservations = static_cast<std::uint32_t>(observations.size());
        report.inputFrames = static_cast<std::uint32_t>(input.frameKeys.size());
        report.inputBlake3 = input.inputBlake3;
        report.datasetGroups = input.datasetGroups;
        report.splitIsolationValid = true;
        report.selectionUsedHoldout = false;
        report.acceptanceAuthorityChanged = false;
        report.productionDefaultsChanged = false;
        for (const std::string& policyId : input.policyIds)
        {
            std::array<MetricModel, 4> models{};
            models[0].kind = MetricModelKind::Raw;
            models[1] = FitGlobalModel(input, policyId);
            models[2] = FitPerSignalModel(input, policyId);
            models[3] = FitPiecewiseModel(input, policyId);
            for (MetricModel& model : models)
            {
                CandidateReport candidate;
                candidate.policyId = policyId;
                candidate.policyManifestJson = input.policyManifests.at(policyId);
                candidate.policyRelaxesDefaultAdmission = input.policyAdmissionRelaxation.at(policyId);
                candidate.model = std::move(model);
                candidate.candidateId = policyId + "/" + ModelSuffix(candidate.model.kind);
                candidate.deployableWithoutSignalProfileBinding =
                    candidate.model.kind != MetricModelKind::PerSignalScale;
                candidate.train = Evaluate(input, policyId, candidate.model, DatasetSplit::Train);
                candidate.validation = Evaluate(input, policyId, candidate.model, DatasetSplit::Validation);
                report.candidates.push_back(std::move(candidate));
            }
        }
        report.baselineCandidateId = std::string(kDefaultPolicyId) + "/Raw";
        const auto baselineIterator = std::ranges::find_if(report.candidates, [&](const CandidateReport& candidate)
        {
            return candidate.candidateId == report.baselineCandidateId;
        });
        if (baselineIterator == report.candidates.end() || !baselineIterator->validation.IsAcceptanceSafe())
        {
            error = "default/raw validation baseline is missing or unsafe";
            return false;
        }
        std::size_t selectedIndex = static_cast<std::size_t>(std::distance(report.candidates.begin(), baselineIterator));
        for (std::size_t index = 0; index < report.candidates.size(); index++)
        {
            if (report.candidates[index].deployableWithoutSignalProfileBinding &&
                !report.candidates[index].policyRelaxesDefaultAdmission &&
                BetterOnValidation(report.candidates[index], report.candidates[selectedIndex]))
            {
                selectedIndex = index;
            }
        }
        report.selectedCandidateId = report.candidates[selectedIndex].candidateId;
        const std::size_t baselineIndex = static_cast<std::size_t>(std::distance(report.candidates.begin(), baselineIterator));
        report.candidates[baselineIndex].holdout = Evaluate(input, report.candidates[baselineIndex].policyId,
            report.candidates[baselineIndex].model, DatasetSplit::Holdout);
        report.candidates[baselineIndex].external = Evaluate(input, report.candidates[baselineIndex].policyId,
            report.candidates[baselineIndex].model, DatasetSplit::External);
        if (selectedIndex != baselineIndex)
        {
            report.candidates[selectedIndex].holdout = Evaluate(input, report.candidates[selectedIndex].policyId,
                report.candidates[selectedIndex].model, DatasetSplit::Holdout);
            report.candidates[selectedIndex].external = Evaluate(input, report.candidates[selectedIndex].policyId,
                report.candidates[selectedIndex].model, DatasetSplit::External);
        }
        const CandidateReport& baseline = report.candidates[baselineIndex];
        const CandidateReport& selected = report.candidates[selectedIndex];
        report.holdoutImproved = ImprovedWithoutRegression(selected.holdout, baseline.holdout, true);
        report.externalImproved = ImprovedWithoutRegression(selected.external, baseline.external, true);
        report.gatePassed = report.splitIsolationValid && !report.selectionUsedHoldout &&
            !report.acceptanceAuthorityChanged && !report.productionDefaultsChanged &&
            selected.train.IsAcceptanceSafe() && selected.validation.IsAcceptanceSafe() &&
            selected.holdout.IsAcceptanceSafe() && selected.external.IsAcceptanceSafe() &&
            !selected.policyRelaxesDefaultAdmission &&
            report.holdoutImproved && report.externalImproved;
        const std::string payload = SerializePayload(report);
        report.canonicalJson = SealPayload(payload);
        output = std::move(report);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown metric calibration failure";
    }
    return false;
}

const char* GetDatasetSplitName(const DatasetSplit split) noexcept
{
    switch (split)
    {
    case DatasetSplit::Train: return "Train";
    case DatasetSplit::Validation: return "Validation";
    case DatasetSplit::Holdout: return "Holdout";
    case DatasetSplit::External: return "External";
    }
    return "Unknown";
}

const char* GetTruthAvailabilityName(const TruthAvailability availability) noexcept
{
    switch (availability)
    {
    case TruthAvailability::DiagnosticSenderFixture: return "DiagnosticSenderFixture";
    case TruthAvailability::UnavailableReceiverOnly: return "UnavailableReceiverOnly";
    }
    return "Unknown";
}

const char* GetMetricModelKindName(const MetricModelKind kind) noexcept
{
    switch (kind)
    {
    case MetricModelKind::Raw: return "Raw";
    case MetricModelKind::GlobalScale: return "GlobalScale";
    case MetricModelKind::PerSignalScale: return "PerSignalScale";
    case MetricModelKind::PiecewiseLookup: return "PiecewiseLookup";
    }
    return "Unknown";
}

} // namespace pbremotevisualmetriccalibration
