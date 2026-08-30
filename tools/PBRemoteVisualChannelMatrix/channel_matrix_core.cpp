#include "channel_matrix_core.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbremotevisualsimulator/channel_transform.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbremotevisualmatrix
{
namespace
{

constexpr std::uint64_t kSessionTag = 0x42A17C9E5D8036BFULL;
constexpr std::uint64_t kPreviousFrameSequence = 700;
constexpr std::uint64_t kCurrentFrameSequence = 701;
constexpr std::uint64_t kMatrixSeed = 0x504252564D415452ULL;

struct ChannelMatrixCaseEvidence
{
    ChannelMatrixCaseSummary summary;
    std::uint64_t seed = 0;
    std::uint32_t manifestVersion = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::size_t outputRowPitch = 0;
    std::string canonicalManifestJson;
    pbdesktoplevels::RemoteVisualLowFpsReferenceObservation observation;
};

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
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
            output.push_back(kHexDigits[character >> 4]);
            output.push_back(kHexDigits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

void AppendUint64HexString(std::string& output, const std::uint64_t value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t nibble = 0; nibble < 16; nibble++)
    {
        const std::size_t shift = (15 - nibble) * 4;
        output.push_back(kHexDigits[(value >> shift) & 0x0FU]);
    }
    output.push_back('"');
}

void AppendBinary64(std::string& output, const double value)
{
    const double normalized = value == 0.0 ? 0.0 : value;
    AppendUint64HexString(output, std::bit_cast<std::uint64_t>(normalized));
}

std::string DigestToHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    return pbremotevisualsimulator::ChannelDigestToHex(digest);
}

std::array<std::byte, pbprotocol::kDigestBytes> HashBytes(const std::span<const std::byte> bytes)
{
    return pbprotocol::ComputeBlake3Digest(bytes);
}

std::array<std::byte, pbprotocol::kDigestBytes> HashString(const std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return HashBytes({bytes, text.size()});
}

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrapRecord(const std::uint64_t frameSequence)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = kSessionTag;
    record.frameSequence = frameSequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    if (!pbprotocol::SerializeBootstrapRecord(record, bytes))
    {
        throw std::runtime_error("Bootstrap serialization failed");
    }
    return bytes;
}

std::vector<std::byte> MakeDiagnosticData(const std::span<const std::byte> record)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    if (!pbdesktoplevels::GenerateDiagnosticData(record, data))
    {
        throw std::runtime_error("diagnostic data generation failed");
    }
    return data;
}

std::vector<std::byte> MakeRaster(const std::span<const std::byte> record, const std::span<const std::byte> data)
{
    const std::size_t rasterBytes = static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4;
    std::vector<std::byte> raster(rasterBytes);
    if (!pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, raster))
    {
        throw std::runtime_error("LF4 raster encoding failed");
    }
    return raster;
}

pbremotevisualsimulator::BgraImageView MakeRasterView(const std::vector<std::byte>& pixels)
{
    return {pixels, pbmodulation::kLocalDesktopCanvasWidth, pbmodulation::kLocalDesktopCanvasHeight,
        static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) * 4};
}

pbremotevisualsimulator::BlockReplacementTransform FindStaleRegionTransform()
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        if (!pbmodulation::GetRemoteVisualTileMapping(physical, mapping) ||
            mapping.role != pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        bool previousBit = false;
        bool currentBit = false;
        if (!pbmodulation::GetRemoteVisualFreshnessBit(kSessionTag, kPreviousFrameSequence, physical, previousBit) ||
            !pbmodulation::GetRemoteVisualFreshnessBit(kSessionTag, kCurrentFrameSequence, physical, currentBit))
        {
            throw std::runtime_error("freshness mapping failed");
        }
        if (previousBit != currentBit)
        {
            const std::uint32_t regionColumn = mapping.regionId % pbmodulation::kRemoteVisualFreshnessRegionColumns;
            const std::uint32_t regionRow = mapping.regionId / pbmodulation::kRemoteVisualFreshnessRegionColumns;
            return {96 + regionColumn * 128, 96 + regionRow * 128, 128, 128};
        }
    }
    throw std::runtime_error("no deterministic stale region was found");
}

ChannelMatrixClassification Classify(const pbdesktoplevels::RemoteVisualLowFpsReferenceObservation& observation) noexcept
{
    if (observation.evaluation.falseAcceptedCodewords != 0)
    {
        return ChannelMatrixClassification::FalseAcceptance;
    }
    if (observation.evaluation.IsVerified())
    {
        return ChannelMatrixClassification::Verified;
    }
    if (observation.modulation.IsAccepted())
    {
        return ChannelMatrixClassification::RejectedNoFalseAccept;
    }
    return ChannelMatrixClassification::ErasureNoFalseAccept;
}

bool MatchesExpectation(const std::string_view expected, const ChannelMatrixClassification classification) noexcept
{
    if (expected == "NoFalseAcceptance")
    {
        return classification != ChannelMatrixClassification::FalseAcceptance &&
            classification != ChannelMatrixClassification::ExecutionFailure;
    }
    return expected == GetChannelMatrixClassificationName(classification);
}

void AppendEvaluation(std::string& output, const pbdesktoplevels::FrameEvaluation& evaluation)
{
    output.append("{\"evaluated\":");
    AppendBoolean(output, evaluation.evaluated);
    output.append(",\"paddingValid\":");
    AppendBoolean(output, evaluation.paddingValid);
    output.append(",\"codewords\":");
    AppendUnsigned(output, evaluation.codewords);
    output.append(",\"fecFailures\":");
    AppendUnsigned(output, evaluation.fecFailures);
    output.append(",\"crcFailures\":");
    AppendUnsigned(output, evaluation.crcFailures);
    output.append(",\"identityFailures\":");
    AppendUnsigned(output, evaluation.identityFailures);
    output.append(",\"falseAcceptedCodewords\":");
    AppendUnsigned(output, evaluation.falseAcceptedCodewords);
    output.append(",\"acceptedTransportBlocks\":");
    AppendUnsigned(output, evaluation.acceptedTransportBlocks);
    output.append(",\"acceptedRemoteControlBlocks\":");
    AppendUnsigned(output, evaluation.acceptedRemoteControlBlocks);
    output.append(",\"iterationsTotal\":");
    AppendUnsigned(output, evaluation.iterationsTotal);
    output.append(",\"iterationsMaximum\":");
    AppendUnsigned(output, evaluation.iterationsMaximum);
    output.append(",\"comparedCodedBits\":");
    AppendUnsigned(output, evaluation.comparedCodedBits);
    output.append(",\"erroneousCodedBits\":");
    AppendUnsigned(output, evaluation.erroneousCodedBits);
    output.push_back('}');
}

void AppendModulation(std::string& output, const pbmodulation::RemoteVisualLowFpsObservation& modulation)
{
    output.append("{\"accepted\":");
    AppendBoolean(output, modulation.IsAccepted());
    output.append(",\"erasure\":");
    AppendJsonString(output, pbmodulation::GetRemoteVisualLowFpsErasureName(modulation.erasure));
    output.append(",\"bootstrap\":{\"accepted\":");
    AppendBoolean(output, modulation.bootstrap.IsAccepted());
    output.append(",\"erasure\":");
    AppendJsonString(output, pbmodulation::GetLocalDesktopErasureName(modulation.bootstrap.erasure));
    output.append(",\"originXBinary64\":");
    AppendBinary64(output, modulation.bootstrap.geometry.originX);
    output.append(",\"originYBinary64\":");
    AppendBinary64(output, modulation.bootstrap.geometry.originY);
    output.append(",\"scaleXBinary64\":");
    AppendBinary64(output, modulation.bootstrap.geometry.scaleX);
    output.append(",\"scaleYBinary64\":");
    AppendBinary64(output, modulation.bootstrap.geometry.scaleY);
    output.append(",\"markerResidualPixelsBinary64\":");
    AppendBinary64(output, modulation.bootstrap.geometry.markerResidualPixels);
    output.append(",\"qualityBinary64\":");
    AppendBinary64(output, modulation.bootstrap.quality);
    output.append(",\"workUnits\":");
    AppendUnsigned(output, modulation.bootstrap.workUnits);
    output.append("},\"calibration\":{\"separationBinary64\":");
    AppendBinary64(output, modulation.calibration.separation);
    output.append(",\"spatialDeviationBinary64\":");
    AppendBinary64(output, modulation.calibration.spatialDeviation);
    output.append("},\"margin\":{\"samples\":");
    AppendUnsigned(output, modulation.margin.samples);
    output.append(",\"minimumBinary64\":");
    AppendBinary64(output, modulation.margin.minimum);
    output.append(",\"p50Binary64\":");
    AppendBinary64(output, modulation.margin.p50);
    output.append(",\"p01Binary64\":");
    AppendBinary64(output, modulation.margin.p01);
    output.append(",\"p001Binary64\":");
    AppendBinary64(output, modulation.margin.p001);
    output.append("},\"dataBytes\":");
    AppendUnsigned(output, modulation.dataBytes);
    output.append(",\"unreliableSymbols\":");
    AppendUnsigned(output, modulation.unreliableSymbols);
    output.append(",\"freshnessRegions\":");
    AppendUnsigned(output, modulation.freshnessRegions);
    output.append(",\"staleRegions\":");
    AppendUnsigned(output, modulation.staleRegions);
    output.append(",\"freshnessTagMismatches\":");
    AppendUnsigned(output, modulation.freshnessTagMismatches);
    output.append(",\"freshnessTagErasures\":");
    AppendUnsigned(output, modulation.freshnessTagErasures);
    output.append(",\"erasedDataMetrics\":");
    AppendUnsigned(output, modulation.erasedDataMetrics);
    output.append(",\"dataWorkUnits\":");
    AppendUnsigned(output, modulation.dataWorkUnits);
    output.append(",\"pixelError\":");
    AppendJsonString(output, pbmodulation::GetLocalDesktopErasureName(modulation.pixelError));
    output.push_back('}');
}

std::string SerializePayload(const std::array<std::byte, pbprotocol::kDigestBytes>& previousRasterBlake3,
    const std::array<std::byte, pbprotocol::kDigestBytes>& currentRasterBlake3,
    const std::vector<ChannelMatrixCaseEvidence>& cases, const bool truthBoundaryValid,
    const bool expectationsMatched)
{
    std::uint64_t verifiedCases = 0;
    std::uint64_t erasureCases = 0;
    std::uint64_t rejectedCases = 0;
    std::uint64_t falseAcceptedCodewords = 0;
    std::uint64_t expectationMismatches = 0;
    for (const ChannelMatrixCaseEvidence& evidence : cases)
    {
        if (evidence.summary.classification == ChannelMatrixClassification::Verified)
        {
            verifiedCases++;
        }
        else if (evidence.summary.classification == ChannelMatrixClassification::ErasureNoFalseAccept)
        {
            erasureCases++;
        }
        else if (evidence.summary.classification == ChannelMatrixClassification::RejectedNoFalseAccept)
        {
            rejectedCases++;
        }
        falseAcceptedCodewords += evidence.summary.falseAcceptedCodewords;
        if (!evidence.summary.expectationMatched)
        {
            expectationMismatches++;
        }
    }

    std::string output;
    output.reserve(64 * 1024);
    output.append("{\"source\":{\"profile\":\"PB-RemoteVisual-LF4-X1\",\"profileId\":");
    AppendUint64HexString(output, pbmodulation::kRemoteVisualLowFpsProfileId);
    output.append(",\"layoutVersion\":");
    AppendUnsigned(output, pbmodulation::kRemoteVisualLowFpsLayoutVersion);
    output.append(",\"sessionTag\":");
    AppendUint64HexString(output, kSessionTag);
    output.append(",\"previousFrameSequence\":");
    AppendUnsigned(output, kPreviousFrameSequence);
    output.append(",\"currentFrameSequence\":");
    AppendUnsigned(output, kCurrentFrameSequence);
    output.append(",\"width\":");
    AppendUnsigned(output, pbmodulation::kLocalDesktopCanvasWidth);
    output.append(",\"height\":");
    AppendUnsigned(output, pbmodulation::kLocalDesktopCanvasHeight);
    output.append(",\"rawRasterHashDomain\":\"tight-bgra8-bytes\",\"previousRawRasterBlake3\":");
    AppendJsonString(output, DigestToHex(previousRasterBlake3));
    output.append(",\"currentRawRasterBlake3\":");
    AppendJsonString(output, DigestToHex(currentRasterBlake3));
    output.append("},\"simulatorPolicy\":{\"maximumDimension\":");
    AppendUnsigned(output, pbremotevisualsimulator::kMaximumChannelDimension);
    output.append(",\"maximumPixels\":");
    AppendUnsigned(output, pbremotevisualsimulator::kMaximumChannelPixels);
    output.append(",\"maximumResidentBytes\":");
    AppendUnsigned(output, pbremotevisualsimulator::kMaximumChannelResidentBytes);
    output.append(",\"maximumWorkUnits\":");
    AppendUnsigned(output, pbremotevisualsimulator::kMaximumChannelWorkUnits);
    output.append(",\"maximumTransforms\":");
    AppendUnsigned(output, pbremotevisualsimulator::kMaximumChannelTransforms);
    output.append("},\"cases\":[");
    for (std::size_t index = 0; index < cases.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const ChannelMatrixCaseEvidence& evidence = cases[index];
        output.append("{\"index\":");
        AppendUnsigned(output, index);
        output.append(",\"name\":");
        AppendJsonString(output, evidence.summary.name);
        output.append(",\"impairmentClass\":");
        AppendJsonString(output, evidence.summary.impairmentClass);
        output.append(",\"expected\":");
        AppendJsonString(output, evidence.summary.expected);
        output.append(",\"classification\":");
        AppendJsonString(output, GetChannelMatrixClassificationName(evidence.summary.classification));
        output.append(",\"expectationMatched\":");
        AppendBoolean(output, evidence.summary.expectationMatched);
        output.append(",\"seed\":");
        AppendUint64HexString(output, evidence.seed);
        output.append(",\"manifestVersion\":");
        AppendUnsigned(output, evidence.manifestVersion);
        output.append(",\"manifestBlake3\":");
        AppendJsonString(output, evidence.summary.manifestBlake3);
        output.append(",\"outputBlake3\":");
        AppendJsonString(output, evidence.summary.outputBlake3);
        output.append(",\"outputGeometry\":{\"width\":");
        AppendUnsigned(output, evidence.outputWidth);
        output.append(",\"height\":");
        AppendUnsigned(output, evidence.outputHeight);
        output.append(",\"rowPitch\":");
        AppendUnsigned(output, evidence.outputRowPitch);
        output.append("},\"channelManifest\":");
        output.append(evidence.canonicalManifestJson);
        output.append(",\"modulation\":");
        AppendModulation(output, evidence.observation.modulation);
        output.append(",\"evaluation\":");
        AppendEvaluation(output, evidence.observation.evaluation);
        output.push_back('}');
    }
    output.append("],\"summary\":{\"caseCount\":");
    AppendUnsigned(output, cases.size());
    output.append(",\"verifiedCases\":");
    AppendUnsigned(output, verifiedCases);
    output.append(",\"erasureCases\":");
    AppendUnsigned(output, erasureCases);
    output.append(",\"rejectedCases\":");
    AppendUnsigned(output, rejectedCases);
    output.append(",\"falseAcceptedCodewords\":");
    AppendUnsigned(output, falseAcceptedCodewords);
    output.append(",\"expectationMismatches\":");
    AppendUnsigned(output, expectationMismatches);
    output.append(",\"truthBoundaryValid\":");
    AppendBoolean(output, truthBoundaryValid);
    output.append(",\"expectationsMatched\":");
    AppendBoolean(output, expectationsMatched);
    output.append("}}");
    return output;
}

} // namespace

const char* GetChannelMatrixClassificationName(const ChannelMatrixClassification classification) noexcept
{
    switch (classification)
    {
    case ChannelMatrixClassification::Verified:
        return "Verified";
    case ChannelMatrixClassification::ErasureNoFalseAccept:
        return "ErasureNoFalseAccept";
    case ChannelMatrixClassification::RejectedNoFalseAccept:
        return "RejectedNoFalseAccept";
    case ChannelMatrixClassification::FalseAcceptance:
        return "FalseAcceptance";
    case ChannelMatrixClassification::ExecutionFailure:
        return "ExecutionFailure";
    }
    return "ExecutionFailure";
}

bool BuildDefaultChannelMatrix(ChannelMatrixReport& output, std::string& error)
{
    error.clear();
    try
    {
        const auto previousRecord = MakeBootstrapRecord(kPreviousFrameSequence);
        const auto currentRecord = MakeBootstrapRecord(kCurrentFrameSequence);
        const auto previousData = MakeDiagnosticData(previousRecord);
        const auto currentData = MakeDiagnosticData(currentRecord);
        const auto previousRaster = MakeRaster(previousRecord, previousData);
        const auto currentRaster = MakeRaster(currentRecord, currentData);
        const auto previousRasterBlake3 = HashBytes(previousRaster);
        const auto currentRasterBlake3 = HashBytes(currentRaster);
        const auto staleBlock = FindStaleRegionTransform();
        std::vector<ChannelMatrixCaseEvidence> cases;
        cases.reserve(14);

        const auto AddCase = [&](const std::string_view name, const std::string_view impairmentClass,
            const std::string_view expected, const std::initializer_list<pbremotevisualsimulator::ChannelTransform> transforms,
            const bool useReference)
        {
            const std::vector<pbremotevisualsimulator::ChannelTransform> transformStorage(transforms);
            const std::uint64_t seed = kMatrixSeed + cases.size();
            const std::optional<pbremotevisualsimulator::BgraImageView> reference = useReference ?
                std::optional<pbremotevisualsimulator::BgraImageView>{MakeRasterView(previousRaster)} : std::nullopt;
            auto executionResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(MakeRasterView(currentRaster),
                reference, {seed, transformStorage});
            if (!executionResult)
            {
                throw std::runtime_error(std::string("channel transform failed for ") + std::string(name) + ": " +
                    pbremotevisualsimulator::GetChannelTransformErrorName(executionResult.Error().code));
            }
            auto execution = std::move(executionResult).Value();
            auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
            if (!channelResult)
            {
                throw std::runtime_error("reference channel allocation failed");
            }
            auto channel = std::move(channelResult).Value();
            const pbmodulation::LumaView view{execution.output.pixels, execution.output.width, execution.output.height,
                execution.output.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
            const auto observation = channel.DecodeRemoteVisualLowFps(view);
            ChannelMatrixCaseEvidence evidence;
            evidence.summary.name = name;
            evidence.summary.impairmentClass = impairmentClass;
            evidence.summary.expected = expected;
            evidence.summary.classification = Classify(observation);
            evidence.summary.expectationMatched = MatchesExpectation(expected, evidence.summary.classification);
            evidence.summary.modulationAccepted = observation.modulation.IsAccepted();
            evidence.summary.evaluationVerified = observation.evaluation.IsVerified();
            evidence.summary.falseAcceptedCodewords = observation.evaluation.falseAcceptedCodewords;
            evidence.summary.acceptedTransportBlocks = observation.evaluation.acceptedTransportBlocks;
            evidence.summary.staleRegions = observation.modulation.staleRegions;
            evidence.summary.erasure = pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure);
            evidence.summary.manifestBlake3 = pbremotevisualsimulator::ChannelDigestToHex(execution.manifestBlake3);
            evidence.summary.outputBlake3 = pbremotevisualsimulator::ChannelDigestToHex(execution.outputBlake3);
            evidence.seed = seed;
            evidence.manifestVersion = execution.manifestVersion;
            evidence.outputWidth = execution.output.width;
            evidence.outputHeight = execution.output.height;
            evidence.outputRowPitch = execution.output.rowPitch;
            evidence.canonicalManifestJson = std::move(execution.canonicalManifestJson);
            evidence.observation = observation;
            cases.push_back(std::move(evidence));
        };

        AddCase("identity", "identity", "Verified", {}, false);
        AddCase("area-upscale", "geometry-scale", "NoFalseAcceptance",
            {pbremotevisualsimulator::ResampleTransform{2442, 1384, 1.259375, 1.2592592592592593, 11.25, 13.5,
                pbremotevisualsimulator::ResampleFilter::Area,
                {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}}}, false);
        AddCase("bilinear-fractional-phase", "geometry-filter-phase", "NoFalseAcceptance",
            {pbremotevisualsimulator::ResampleTransform{1932, 1092, 1.0, 1.0, 6.25, 6.5,
                pbremotevisualsimulator::ResampleFilter::Bilinear,
                {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}}}, false);
        AddCase("letterbox-075", "geometry-letterbox", "NoFalseAcceptance",
            {pbremotevisualsimulator::ResampleTransform{1600, 900, 0.75, 0.75, 80.0, 45.0,
                pbremotevisualsimulator::ResampleFilter::Area,
                {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}}}, false);
        AddCase("box-blur-1", "spatial-blur", "NoFalseAcceptance",
            {pbremotevisualsimulator::Kernel3x3Transform{pbremotevisualsimulator::FixedKernel3x3::BoxBlur, 1}}, false);
        AddCase("gaussian-blur-1", "spatial-blur", "Verified",
            {pbremotevisualsimulator::Kernel3x3Transform{pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, 1}}, false);
        AddCase("sharpen-1", "spatial-sharpen", "NoFalseAcceptance",
            {pbremotevisualsimulator::Kernel3x3Transform{pbremotevisualsimulator::FixedKernel3x3::Sharpen, 1}}, false);
        AddCase("limited-range", "color-range", "Verified",
            {pbremotevisualsimulator::ColorTransferTransform{219.0 / 255.0, 16.0, 1.0}}, false);
        AddCase("gamma-115", "color-gamma", "NoFalseAcceptance",
            {pbremotevisualsimulator::ColorTransferTransform{1.0, 0.0, 1.15}}, false);
        AddCase("chroma-420", "chroma-subsampling", "Verified",
            {pbremotevisualsimulator::ChromaSubsample420Transform{}}, false);
        AddCase("illegal-crop-8px", "geometry-crop", "ErasureNoFalseAccept",
            {pbremotevisualsimulator::CropTransform{8, 0, pbmodulation::kLocalDesktopCanvasWidth - 8,
                pbmodulation::kLocalDesktopCanvasHeight}}, false);
        AddCase("codec-block-overlay", "spatial-overlay", "Verified",
            {pbremotevisualsimulator::SolidOverlayTransform{96 + 4 * 128, 96 + 3 * 128, 128, 128,
                {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}, 255}}, false);
        AddCase("stale-region-replacement", "temporal-block-replacement", "Verified", {staleBlock}, true);
        AddCase("temporal-blend-96", "temporal-blend", "ErasureNoFalseAccept",
            {pbremotevisualsimulator::ReferenceBlendTransform{96}}, true);

        ChannelMatrixReport report;
        report.cases.reserve(cases.size());
        report.truthBoundaryValid = true;
        report.expectationsMatched = true;
        for (const ChannelMatrixCaseEvidence& evidence : cases)
        {
            report.cases.push_back(evidence.summary);
            report.truthBoundaryValid = report.truthBoundaryValid && evidence.summary.falseAcceptedCodewords == 0 &&
                evidence.summary.classification != ChannelMatrixClassification::ExecutionFailure;
            report.expectationsMatched = report.expectationsMatched && evidence.summary.expectationMatched;
        }
        const std::string payload = SerializePayload(previousRasterBlake3, currentRasterBlake3, cases,
            report.truthBoundaryValid, report.expectationsMatched);
        report.payloadBlake3 = HashString(payload);
        report.canonicalJson.reserve(payload.size() + 160);
        report.canonicalJson.append("{\"schema\":");
        AppendJsonString(report.canonicalJson, kChannelMatrixSchema);
        report.canonicalJson.append(",\"version\":");
        AppendUnsigned(report.canonicalJson, kChannelMatrixVersion);
        report.canonicalJson.append(",\"payloadBlake3\":");
        AppendJsonString(report.canonicalJson, DigestToHex(report.payloadBlake3));
        report.canonicalJson.append(",\"payload\":");
        report.canonicalJson.append(payload);
        report.canonicalJson.append("}\n");
        output = std::move(report);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown matrix generation failure";
    }
    return false;
}

} // namespace pbremotevisualmatrix
