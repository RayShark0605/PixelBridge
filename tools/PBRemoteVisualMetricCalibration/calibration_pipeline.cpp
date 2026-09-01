#include "calibration_pipeline.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbrealcapturereplay/replay_v2.h"
#include "pbremotevisualsimulator/channel_transform.h"
#include "receiver_evidence.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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

inline constexpr std::uint64_t kPresenterSessionTag = 0x5354455030325244ULL;
inline constexpr std::uint64_t kPresenterFrameSequence = 17;
inline constexpr std::uint64_t kSimulatorSeed = 0x504252564D43414CULL;
inline constexpr std::uint64_t kGrayFrameBytes =
    static_cast<std::uint64_t>(pbmodulation::kLocalDesktopCanvasWidth) *
    pbmodulation::kLocalDesktopCanvasHeight;

struct PolicyBinding
{
    std::string id;
    std::string manifest;
    pbmodulation::RemoteVisualLowFpsDecodePolicy policy;
    bool relaxesDefaultAdmission = false;
};

struct DecoderBinding
{
    PolicyBinding binding;
    pbmodulation::RemoteVisualLowFpsWorkspace workspace;
    std::array<std::byte, pbmodulation::kRemoteVisualLowFpsDataBytes> hard{};
    std::vector<float> soft;
};

struct SimulatorCase
{
    const char* runId = nullptr;
    DatasetSplit split = DatasetSplit::Train;
    std::uint64_t frameSequence = 0;
    std::vector<pbremotevisualsimulator::ChannelTransform> transforms;
    bool usePreviousReference = false;
};

struct FileSnapshot
{
    std::uint64_t bytes = 0;
    std::filesystem::file_time_type lastWriteTime{};
    std::string blake3;
};

[[nodiscard]] bool IsLowerHexDigest(const std::string_view value) noexcept
{
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool IsPortableIdentifier(const std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 128 &&
        std::ranges::all_of(value, [](const unsigned char character)
        {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.';
        });
}

void ValidatePipelineInput(const CalibrationPipelineInput& input)
{
    constexpr std::uint64_t maximumInMemoryFileBytes = 512ULL * 1024 * 1024;
    constexpr std::uint64_t maximumAggregateCodecBytes = 2ULL * 1024 * 1024 * 1024;
    if (input.codecSequences.empty() || input.codecSequences.size() > 16 ||
        input.maximumCodecFileBytes < kGrayFrameBytes ||
        input.maximumCodecFileBytes > maximumInMemoryFileBytes ||
        input.maximumTotalCodecBytes < input.maximumCodecFileBytes ||
        input.maximumTotalCodecBytes > maximumAggregateCodecBytes ||
        input.maximumReplayBytes < 16ULL * 1024 * 1024 ||
        input.maximumReplayBytes > maximumInMemoryFileBytes || input.maximumReplayFrames == 0 ||
        input.maximumReplayFrames > pbrealcapturereplay::kReplayV2HardMaximumCaptureFrames)
    {
        throw std::runtime_error("metric calibration resource policy is outside its fixed in-memory bounds");
    }
    std::set<std::string> runIds;
    std::array<std::uint32_t, 3> splitRuns{};
    for (const CodecSequenceInput& sequence : input.codecSequences)
    {
        if ((sequence.split != DatasetSplit::Train && sequence.split != DatasetSplit::Validation &&
            sequence.split != DatasetSplit::Holdout) || !IsPortableIdentifier(sequence.signalProfile) ||
            !IsPortableIdentifier(sequence.runId) || sequence.gray8Path.empty() ||
            !IsLowerHexDigest(sequence.expectedBlake3) || !runIds.insert(sequence.runId).second)
        {
            throw std::runtime_error("codec sequence manifest is invalid before calibration execution");
        }
        splitRuns[static_cast<std::size_t>(sequence.split)]++;
    }
    if (std::ranges::any_of(splitRuns, [](const std::uint32_t value) { return value == 0; }))
    {
        throw std::runtime_error("codec sequence manifest must cover Train, Validation and Holdout");
    }
    const RealReplayInput& replay = input.realReplay;
    const auto isDatasetId = [](const std::string_view value)
    {
        return value.size() == 32 && std::ranges::all_of(value, [](const unsigned char character)
        {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
    };
    if (!isDatasetId(replay.collectionDatasetId) || replay.replayPath.empty() ||
        !IsLowerHexDigest(replay.expectedReplayBlake3) ||
        !IsLowerHexDigest(replay.expectedPresenterRasterBlake3))
    {
        throw std::runtime_error("real Replay manifest is invalid before calibration execution");
    }
}

[[nodiscard]] std::string BytesToHex(const std::span<const std::byte> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned value = std::to_integer<unsigned>(bytes[index]);
        output[index * 2] = digits[value >> 4];
        output[index * 2 + 1] = digits[value & 0x0F];
    }
    return output;
}

[[nodiscard]] std::string HashBytes(const std::span<const std::byte> bytes)
{
    return BytesToHex(pbprotocol::ComputeBlake3Digest(bytes));
}

[[nodiscard]] bool ReadBoundedFile(const std::filesystem::path& path, const std::uint64_t maximumBytes,
    std::vector<std::byte>& output, FileSnapshot& snapshot, std::string& error)
{
    try
    {
        std::error_code metadataError;
        const std::uint64_t bytes = std::filesystem::file_size(path, metadataError);
        if (metadataError || bytes == 0 || bytes > maximumBytes ||
            bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        {
            error = "input file size is unavailable or outside its bounded policy";
            return false;
        }
        const auto beforeWriteTime = std::filesystem::last_write_time(path, metadataError);
        if (metadataError)
        {
            error = "input file timestamp is unavailable";
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "cannot open bounded input file";
            return false;
        }
        std::vector<std::byte> contents(static_cast<std::size_t>(bytes));
        input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size()) || input.bad())
        {
            error = "bounded input file was truncated while reading";
            return false;
        }
        char trailing = 0;
        input.read(&trailing, 1);
        if (input.gcount() != 0)
        {
            error = "bounded input file grew while reading";
            return false;
        }
        const std::uint64_t afterBytes = std::filesystem::file_size(path, metadataError);
        if (metadataError || afterBytes != bytes ||
            std::filesystem::last_write_time(path, metadataError) != beforeWriteTime || metadataError)
        {
            error = "bounded input file changed while reading";
            return false;
        }
        FileSnapshot candidate;
        candidate.bytes = bytes;
        candidate.lastWriteTime = beforeWriteTime;
        candidate.blake3 = HashBytes(contents);
        output = std::move(contents);
        snapshot = std::move(candidate);
        return true;
    }
    catch (...)
    {
        error = "invalid or inaccessible bounded input path";
        return false;
    }
}

[[nodiscard]] bool VerifySnapshotUnchanged(const std::filesystem::path& path,
    const std::uint64_t maximumBytes, const FileSnapshot& expected, std::string& error)
{
    std::vector<std::byte> ignored;
    FileSnapshot current;
    if (!ReadBoundedFile(path, maximumBytes, ignored, current, error))
    {
        return false;
    }
    if (current.bytes != expected.bytes || current.lastWriteTime != expected.lastWriteTime ||
        current.blake3 != expected.blake3)
    {
        error = "input file changed after semantic validation";
        return false;
    }
    return true;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
    const std::uint64_t sessionTag, const std::uint64_t frameSequence)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = frameSequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> output{};
    if (!pbprotocol::SerializeBootstrapRecord(record, output))
    {
        throw std::runtime_error("LF4 calibration Bootstrap serialization failed");
    }
    return output;
}

[[nodiscard]] std::vector<std::byte> MakeRaster(
    const std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& bootstrap)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    if (!pbdesktoplevels::GenerateDiagnosticData(bootstrap, data))
    {
        throw std::runtime_error("LF4 calibration sender data generation failed");
    }
    std::vector<std::byte> raster(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    if (!pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, data, raster))
    {
        throw std::runtime_error("LF4 calibration raster generation failed");
    }
    return raster;
}

[[nodiscard]] std::vector<PolicyBinding> MakePolicies()
{
    std::vector<PolicyBinding> policies;
    policies.reserve(6);
    const pbmodulation::RemoteVisualLowFpsDecodePolicy defaults;
    policies.push_back({kDefaultPolicyId,
        "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
        defaults, false});
    auto marginPermissive = defaults;
    marginPermissive.minimumSymbolMargin = 0.04;
    policies.push_back({"lf4-margin-004",
        "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.040000000000000001,\"minimumSymbolRms\":0.34999999999999998}",
        marginPermissive, true});
    auto marginConservative = defaults;
    marginConservative.minimumSymbolMargin = 0.12;
    policies.push_back({"lf4-margin-012",
        "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.12,\"minimumSymbolRms\":0.34999999999999998}",
        marginConservative, false});
    auto residualConservative = defaults;
    residualConservative.maximumSymbolResidual = 0.55;
    policies.push_back({"lf4-residual-055",
        "{\"maximumSymbolResidual\":0.55000000000000004,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
        residualConservative, false});
    auto residualPermissive = defaults;
    residualPermissive.maximumSymbolResidual = 0.9;
    policies.push_back({"lf4-residual-090",
        "{\"maximumSymbolResidual\":0.90000000000000002,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
        residualPermissive, true});
    auto rmsConservative = defaults;
    rmsConservative.minimumSymbolRms = 0.45;
    policies.push_back({"lf4-rms-045",
        "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.45000000000000001}",
        rmsConservative, false});
    return policies;
}

[[nodiscard]] std::vector<DecoderBinding> MakeDecoders()
{
    std::vector<DecoderBinding> decoders;
    for (PolicyBinding& policy : MakePolicies())
    {
        auto workspaceResult = pbmodulation::RemoteVisualLowFpsWorkspace::Create(
            pbmodulation::RemoteVisualLowFpsWorkspace::RequiredBytes());
        if (!workspaceResult)
        {
            throw std::runtime_error("LF4 calibration workspace allocation failed");
        }
        DecoderBinding decoder;
        decoder.binding = std::move(policy);
        decoder.workspace = std::move(workspaceResult).Value();
        decoder.soft.resize(pbmodulation::kRemoteVisualLowFpsCodedBits);
        decoders.push_back(std::move(decoder));
    }
    return decoders;
}

void DecodeFrame(const std::string_view datasetId, const std::string_view runId,
    const std::string_view frameId, const std::string_view signalProfile, const DatasetSplit split,
    const std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& sourceBootstrap,
    const pbmodulation::LumaView& view, std::vector<DecoderBinding>& decoders,
    std::vector<MetricObservation>& output)
{
    for (DecoderBinding& decoder : decoders)
    {
        const auto modulation = pbmodulation::DecodeRemoteVisualLowFpsFrame(view, decoder.workspace,
            decoder.hard, decoder.soft, decoder.binding.policy);
        MetricObservation observation;
        observation.datasetId = datasetId;
        observation.runId = runId;
        observation.frameId = frameId;
        observation.signalProfile = signalProfile;
        observation.policyId = decoder.binding.id;
        observation.policyManifestJson = decoder.binding.manifest;
        observation.policyRelaxesDefaultAdmission = decoder.binding.relaxesDefaultAdmission;
        observation.split = split;
        observation.truthAvailability = TruthAvailability::DiagnosticSenderFixture;
        observation.sourceBootstrap = sourceBootstrap;
        observation.modulationAccepted = modulation.IsAccepted();
        observation.receiverBootstrapAvailable = modulation.IsAccepted();
        observation.modulationErasure = pbmodulation::GetRemoteVisualLowFpsErasureName(modulation.erasure);
        if (modulation.IsAccepted())
        {
            observation.receiverBootstrap = modulation.bootstrap.canonical44;
            observation.rawMetrics.assign(decoder.soft.begin(), decoder.soft.end());
        }
        output.push_back(std::move(observation));
    }
}

[[nodiscard]] std::vector<SimulatorCase> MakeSimulatorCases()
{
    using pbremotevisualsimulator::ColorTransferTransform;
    using pbremotevisualsimulator::FixedKernel3x3;
    using pbremotevisualsimulator::Kernel3x3Transform;
    using pbremotevisualsimulator::ReferenceBlendTransform;
    using pbremotevisualsimulator::ResampleFilter;
    using pbremotevisualsimulator::ResampleTransform;
    const auto border = std::array<std::byte, 4>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    return {
        {"sim-train-identity", DatasetSplit::Train, 10, {}, false},
        {"sim-train-gaussian-1", DatasetSplit::Train, 11,
            {Kernel3x3Transform{FixedKernel3x3::GaussianBlur, 1}}, false},
        {"sim-train-box-1", DatasetSplit::Train, 12,
            {Kernel3x3Transform{FixedKernel3x3::BoxBlur, 1}}, false},
        {"sim-train-gamma-095", DatasetSplit::Train, 13,
            {ColorTransferTransform{1.0, 0.0, 0.95}}, false},
        {"sim-train-scale-090", DatasetSplit::Train, 14,
            {ResampleTransform{1770, 1000, 0.9, 0.9, 21.25, 14.5, ResampleFilter::Area, border}}, false},
        {"sim-train-blend-032", DatasetSplit::Train, 15,
            {ReferenceBlendTransform{32}}, true},
        {"sim-validation-identity", DatasetSplit::Validation, 20, {}, false},
        {"sim-validation-gaussian-2", DatasetSplit::Validation, 21,
            {Kernel3x3Transform{FixedKernel3x3::GaussianBlur, 2}}, false},
        {"sim-validation-box-2", DatasetSplit::Validation, 22,
            {Kernel3x3Transform{FixedKernel3x3::BoxBlur, 2}}, false},
        {"sim-validation-gamma-110", DatasetSplit::Validation, 23,
            {ColorTransferTransform{1.0, 0.0, 1.10}}, false},
        {"sim-validation-scale-075", DatasetSplit::Validation, 24,
            {ResampleTransform{1600, 900, 0.75, 0.75, 80.0, 45.0, ResampleFilter::Bilinear, border}}, false},
        {"sim-validation-blend-064", DatasetSplit::Validation, 25,
            {ReferenceBlendTransform{64}}, true},
        {"sim-holdout-identity", DatasetSplit::Holdout, 30, {}, false},
        {"sim-holdout-gaussian-3", DatasetSplit::Holdout, 31,
            {Kernel3x3Transform{FixedKernel3x3::GaussianBlur, 3}}, false},
        {"sim-holdout-box-3", DatasetSplit::Holdout, 32,
            {Kernel3x3Transform{FixedKernel3x3::BoxBlur, 3}}, false},
        {"sim-holdout-gamma-120", DatasetSplit::Holdout, 33,
            {ColorTransferTransform{1.0, 0.0, 1.20}}, false},
        {"sim-holdout-scale-1259", DatasetSplit::Holdout, 34,
            {ResampleTransform{2442, 1384, 1.259375, 1.2592592592592593, 11.25, 13.5,
                ResampleFilter::Area, border}}, false},
        {"sim-holdout-blend-096", DatasetSplit::Holdout, 35,
            {ReferenceBlendTransform{96}}, true}};
}

void BuildSimulatorObservations(std::vector<DecoderBinding>& decoders,
    std::vector<MetricObservation>& observations, std::vector<CalibrationArtifactEvidence>& artifacts)
{
    const std::uint64_t sessionTag = pbremotevisualreceiverevidence::GetReceiverEvidenceSessionTag().value;
    const std::vector<SimulatorCase> cases = MakeSimulatorCases();
    for (std::size_t index = 0; index < cases.size(); index++)
    {
        const SimulatorCase& currentCase = cases[index];
        const auto sourceBootstrap = MakeBootstrap(sessionTag, currentCase.frameSequence);
        const auto previousBootstrap = MakeBootstrap(sessionTag, currentCase.frameSequence - 1);
        const std::vector<std::byte> sourceRaster = MakeRaster(sourceBootstrap);
        const std::vector<std::byte> previousRaster = MakeRaster(previousBootstrap);
        const pbremotevisualsimulator::BgraImageView sourceView{sourceRaster,
            pbmodulation::kLocalDesktopCanvasWidth, pbmodulation::kLocalDesktopCanvasHeight,
            static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) * 4};
        const pbremotevisualsimulator::BgraImageView previousView{previousRaster,
            pbmodulation::kLocalDesktopCanvasWidth, pbmodulation::kLocalDesktopCanvasHeight,
            static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) * 4};
        const std::optional<pbremotevisualsimulator::BgraImageView> reference =
            currentCase.usePreviousReference ? std::optional{previousView} : std::nullopt;
        const pbremotevisualsimulator::ChannelTransformPlan plan{
            kSimulatorSeed + index, currentCase.transforms};
        auto executionResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(sourceView, reference, plan);
        if (!executionResult)
        {
            throw std::runtime_error(std::string("Step-07 simulator case failed: ") + currentCase.runId + " error=" +
                pbremotevisualsimulator::GetChannelTransformErrorName(executionResult.Error().code));
        }
        auto execution = std::move(executionResult).Value();
        const pbmodulation::LumaView view{execution.output.pixels, execution.output.width,
            execution.output.height, execution.output.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
        DecodeFrame("step07-simulator-v1", currentCase.runId, "frame-0",
            "PB-Signal-SyntheticBgra-1", currentCase.split, sourceBootstrap, view, decoders, observations);
        artifacts.push_back({"SimulatorManifest", currentCase.runId,
            execution.canonicalManifestJson.size(),
            pbremotevisualsimulator::ChannelDigestToHex(execution.manifestBlake3)});
        artifacts.push_back({"SimulatorOutput", currentCase.runId, execution.output.pixels.size(),
            pbremotevisualsimulator::ChannelDigestToHex(execution.outputBlake3)});
    }
}

void BuildCodecObservations(const CalibrationPipelineInput& input, std::vector<DecoderBinding>& decoders,
    std::vector<MetricObservation>& observations, std::vector<CalibrationArtifactEvidence>& artifacts)
{
    if (input.codecSequences.empty() || input.codecSequences.size() > 16 ||
        input.maximumCodecFileBytes < kGrayFrameBytes || input.maximumTotalCodecBytes < input.maximumCodecFileBytes)
    {
        throw std::runtime_error("codec sequence input policy is empty or invalid");
    }
    std::uint64_t totalBytes = 0;
    std::set<std::string> runIds;
    std::array<std::uint32_t, 3> splitRuns{};
    for (const CodecSequenceInput& sequence : input.codecSequences)
    {
        if ((sequence.split != DatasetSplit::Train && sequence.split != DatasetSplit::Validation &&
            sequence.split != DatasetSplit::Holdout) || sequence.signalProfile.empty() || sequence.runId.empty() ||
            !IsLowerHexDigest(sequence.expectedBlake3) || !runIds.insert(sequence.runId).second)
        {
            throw std::runtime_error("codec sequence split, identity or expected digest is invalid");
        }
        std::vector<std::byte> bytes;
        FileSnapshot snapshot;
        std::string fileError;
        if (!ReadBoundedFile(sequence.gray8Path, input.maximumCodecFileBytes, bytes, snapshot, fileError))
        {
            throw std::runtime_error("codec sequence read failed for " + sequence.runId + ": " + fileError);
        }
        if (snapshot.blake3 != sequence.expectedBlake3 || snapshot.bytes % kGrayFrameBytes != 0)
        {
            throw std::runtime_error("codec sequence digest or exact Gray8 framing disagrees with the sealed input");
        }
        const std::uint64_t nextTotal = totalBytes + snapshot.bytes;
        if (nextTotal < totalBytes || nextTotal > input.maximumTotalCodecBytes)
        {
            throw std::runtime_error("codec sequence aggregate byte policy exceeded");
        }
        totalBytes = nextTotal;
        const std::uint64_t frameCount = snapshot.bytes / kGrayFrameBytes;
        if (frameCount == 0 || frameCount > 64)
        {
            throw std::runtime_error("codec sequence frame count is outside 1..64");
        }
        splitRuns[static_cast<std::size_t>(sequence.split)]++;
        for (std::uint64_t frame = 0; frame < frameCount; frame++)
        {
            std::array<std::byte, pbprotocol::kBootstrapRecordBytes> sourceBootstrap{};
            std::string bootstrapError;
            if (!pbremotevisualreceiverevidence::MakeReceiverEvidenceBootstrapRecord(
                frame, sourceBootstrap, bootstrapError))
            {
                throw std::runtime_error(bootstrapError);
            }
            const auto pixels = std::span(bytes).subspan(static_cast<std::size_t>(frame * kGrayFrameBytes),
                static_cast<std::size_t>(kGrayFrameBytes));
            const pbmodulation::LumaView view{pixels, pbmodulation::kLocalDesktopCanvasWidth,
                pbmodulation::kLocalDesktopCanvasHeight, pbmodulation::kLocalDesktopCanvasWidth,
                pbmodulation::LumaPixelFormat::Gray8};
            DecodeFrame("step06-actual-codec-v2", sequence.runId,
                "frame-" + std::to_string(frame), sequence.signalProfile, sequence.split,
                sourceBootstrap, view, decoders, observations);
        }
        artifacts.push_back({"ActualCodecGray8", sequence.runId, snapshot.bytes, snapshot.blake3});
    }
    if (splitRuns[0] == 0 || splitRuns[1] == 0 || splitRuns[2] == 0)
    {
        throw std::runtime_error("actual codec runs must cover Train, Validation and Holdout");
    }
}

[[nodiscard]] std::optional<pbmodulation::LumaPixelFormat> ResolvePixelFormat(
    const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM: return pbmodulation::LumaPixelFormat::Gray8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return pbmodulation::LumaPixelFormat::Bgra8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return pbmodulation::LumaPixelFormat::R10G10B10A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return pbmodulation::LumaPixelFormat::Fp16LinearSdr;
    default: return std::nullopt;
    }
}

void BuildRealReplayObservations(const CalibrationPipelineInput& input,
    std::vector<DecoderBinding>& decoders, std::vector<MetricObservation>& observations,
    std::vector<CalibrationArtifactEvidence>& artifacts, std::string& presenterRasterBlake3,
    std::string& replayDatasetId, std::string& replayRunId)
{
    const RealReplayInput& replayInput = input.realReplay;
    if (replayInput.collectionDatasetId.empty() ||
        !IsLowerHexDigest(replayInput.expectedReplayBlake3) ||
        !IsLowerHexDigest(replayInput.expectedPresenterRasterBlake3) ||
        input.maximumReplayBytes < 16ULL * 1024 * 1024 || input.maximumReplayFrames == 0 ||
        input.maximumReplayFrames > pbrealcapturereplay::kReplayV2HardMaximumCaptureFrames)
    {
        throw std::runtime_error("real Replay identity, digest or resource policy is invalid");
    }
    const auto sourceBootstrap = MakeBootstrap(kPresenterSessionTag, kPresenterFrameSequence);
    const std::vector<std::byte> presenterRaster = MakeRaster(sourceBootstrap);
    presenterRasterBlake3 = HashBytes(presenterRaster);
    if (presenterRasterBlake3 != replayInput.expectedPresenterRasterBlake3)
    {
        throw std::runtime_error("known Presenter raster does not match its sealed truth digest");
    }
    std::vector<std::byte> replayBytes;
    FileSnapshot before;
    std::string fileError;
    if (!ReadBoundedFile(replayInput.replayPath, input.maximumReplayBytes, replayBytes, before, fileError))
    {
        throw std::runtime_error("real Replay hash preflight failed: " + fileError);
    }
    replayBytes.clear();
    replayBytes.shrink_to_fit();
    if (before.blake3 != replayInput.expectedReplayBlake3)
    {
        throw std::runtime_error("real Replay BLAKE3 disagrees with the sealed dataset index");
    }
    pbrealcapturereplay::ReplayV2Limits limits;
    limits.maximumCaptureFrames = input.maximumReplayFrames;
    limits.maximumFileBytes = input.maximumReplayBytes;
    limits.maximumTotalRasterBytes = input.maximumReplayBytes;
    std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
    const auto openStatus = pbrealcapturereplay::ReplayV2Reader::Open(replayInput.replayPath, limits, reader);
    if (!openStatus)
    {
        throw std::runtime_error("ReplayV2Reader rejected the real LF4 input");
    }
    const auto opened = reader->GetSnapshot();
    replayDatasetId = BytesToHex(opened.descriptor.datasetId);
    replayRunId = opened.descriptor.runId;
    if (opened.descriptor.datasetClass != pbrealcapturereplay::ReplayDatasetClass::RemoteVisual ||
        replayDatasetId.size() != 32 || !IsPortableIdentifier(replayRunId) ||
        opened.descriptor.visualProfileId != pbmodulation::kRemoteVisualLowFpsProfileId)
    {
        throw std::runtime_error("real Replay descriptor does not match the LF4 truth manifest");
    }
    std::uint32_t frame = 0;
    for (;;)
    {
        pbrealcapturereplay::ReplayV2Record record;
        const auto status = reader->ReadNext(record);
        if (!status)
        {
            if (status.code != pbrealcapturereplay::ReplayError::EndOfFile)
            {
                throw std::runtime_error("real Replay record validation failed before its footer");
            }
            break;
        }
        if (record.type != pbrealcapturereplay::ReplayV2RecordType::Capture)
        {
            throw std::runtime_error("capture-only Step-02 Replay unexpectedly contains live demod observations");
        }
        const auto pixelFormat = ResolvePixelFormat(record.capture.capturedRoi.pixelFormat);
        if (!pixelFormat)
        {
            throw std::runtime_error("real Replay uses an unsupported bounded luma format");
        }
        const pbmodulation::LumaView view{record.capture.capturedRoi.pixels,
            record.capture.capturedRoi.width, record.capture.capturedRoi.height,
            record.capture.capturedRoi.rowPitch, *pixelFormat};
        DecodeFrame(replayInput.collectionDatasetId, replayRunId,
            "frame-" + std::to_string(frame), "PB-Signal-RdpUnknown-1", DatasetSplit::External,
            sourceBootstrap, view, decoders, observations);
        frame++;
    }
    const auto closed = reader->GetSnapshot();
    if (!closed.complete || frame == 0 || closed.captureFrames != frame || closed.demodObservations != 0)
    {
        throw std::runtime_error("real Replay did not complete at its capture-only footer boundary");
    }
    if (!VerifySnapshotUnchanged(replayInput.replayPath, input.maximumReplayBytes, before, fileError))
    {
        throw std::runtime_error("real Replay postflight failed: " + fileError);
    }
    artifacts.push_back({"RealReceiverReplayV2", replayRunId, before.bytes, before.blake3});
}

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("calibration evidence integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char digits[] = "0123456789abcdef";
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
            output.push_back(digits[character >> 4]);
            output.push_back(digits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string SerializeEvidencePayload(const CalibrationPipelineReport& report)
{
    std::string output;
    output.reserve(report.calibration.canonicalJson.size() + report.artifacts.size() * 256 + 1024);
    output.append("{\"inputs\":{\"presenterRasterBlake3\":");
    AppendJsonString(output, report.presenterRasterBlake3);
    output.append(",\"realReplayIdentity\":{\"collectionDatasetId\":");
    AppendJsonString(output, report.realCollectionDatasetId);
    output.append(",\"replayDatasetId\":");
    AppendJsonString(output, report.realReplayDatasetId);
    output.append(",\"runId\":");
    AppendJsonString(output, report.realReplayRunId);
    output.append("},\"artifacts\":[");
    for (std::size_t index = 0; index < report.artifacts.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const CalibrationArtifactEvidence& artifact = report.artifacts[index];
        output.append("{\"kind\":");
        AppendJsonString(output, artifact.kind);
        output.append(",\"runId\":");
        AppendJsonString(output, artifact.runId);
        output.append(",\"bytes\":");
        AppendUnsigned(output, artifact.bytes);
        output.append(",\"blake3\":");
        AppendJsonString(output, artifact.blake3);
        output.push_back('}');
    }
    output.append("],\"providerSpecificThresholds\":false,\"realReplayTruthProvenance\":"
        "\"SealedPresenterRasterPlusReplayIdentity\",\"resourcePolicy\":{\"maximumCodecFileBytes\":");
    AppendUnsigned(output, report.maximumCodecFileBytes);
    output.append(",\"maximumTotalCodecBytes\":");
    AppendUnsigned(output, report.maximumTotalCodecBytes);
    output.append(",\"maximumReplayBytes\":");
    AppendUnsigned(output, report.maximumReplayBytes);
    output.append(",\"maximumReplayFrames\":");
    AppendUnsigned(output, report.maximumReplayFrames);
    output.append("}},\"calibration\":");
    std::string_view calibrationJson = report.calibration.canonicalJson;
    while (!calibrationJson.empty() && (calibrationJson.back() == '\n' || calibrationJson.back() == '\r'))
    {
        calibrationJson.remove_suffix(1);
    }
    output.append(calibrationJson);
    output.append(",\"gate\":{\"passed\":");
    AppendBoolean(output, report.calibration.gatePassed);
    output.append(",\"productionDefaultsChanged\":false,\"deploymentDisposition\":"
        "\"CandidateOnlyRequiresStep08Freeze\"}}");
    return output;
}

[[nodiscard]] std::string SealEvidencePayload(const std::string& payload)
{
    const std::string digest = HashBytes(std::as_bytes(std::span(payload.data(), payload.size())));
    std::string output;
    output.reserve(payload.size() + 192);
    output.append("{\"schema\":");
    AppendJsonString(output, kMetricCalibrationEvidenceSchema);
    output.append(",\"version\":");
    AppendUnsigned(output, kMetricCalibrationEvidenceVersion);
    output.append(",\"payloadBlake3\":");
    AppendJsonString(output, digest);
    output.append(",\"payload\":");
    output.append(payload);
    output.append("}\n");
    return output;
}

} // namespace

bool BuildCalibrationEvidence(const CalibrationPipelineInput& input,
    CalibrationPipelineReport& output, std::string& error) noexcept
{
    try
    {
        ValidatePipelineInput(input);
        std::vector<DecoderBinding> decoders = MakeDecoders();
        std::vector<MetricObservation> observations;
        observations.reserve(512);
        CalibrationPipelineReport report;
        report.maximumCodecFileBytes = input.maximumCodecFileBytes;
        report.maximumTotalCodecBytes = input.maximumTotalCodecBytes;
        report.maximumReplayBytes = input.maximumReplayBytes;
        report.maximumReplayFrames = input.maximumReplayFrames;
        report.realCollectionDatasetId = input.realReplay.collectionDatasetId;
        BuildSimulatorObservations(decoders, observations, report.artifacts);
        BuildCodecObservations(input, decoders, observations, report.artifacts);
        BuildRealReplayObservations(input, decoders, observations, report.artifacts,
            report.presenterRasterBlake3, report.realReplayDatasetId, report.realReplayRunId);
        std::ranges::sort(report.artifacts, [](const CalibrationArtifactEvidence& first,
            const CalibrationArtifactEvidence& second)
        {
            return first.kind == second.kind ? first.runId < second.runId : first.kind < second.kind;
        });
        if (!BuildCalibrationReport(observations, report.calibration, error))
        {
            return false;
        }
        const std::string payload = SerializeEvidencePayload(report);
        report.canonicalJson = SealEvidencePayload(payload);
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
        error = "unknown metric calibration evidence failure";
    }
    return false;
}

} // namespace pbremotevisualmetriccalibration
