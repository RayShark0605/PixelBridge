#pragma once

#include "metric_calibration_core.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pbremotevisualmetriccalibration
{

inline constexpr char kMetricCalibrationEvidenceSchema[] =
    "PixelBridge.RemoteVisualMetricCalibrationEvidence.1";
inline constexpr std::uint32_t kMetricCalibrationEvidenceVersion = 1;

struct CodecSequenceInput
{
    DatasetSplit split = DatasetSplit::Train;
    std::string signalProfile;
    std::string runId;
    std::filesystem::path gray8Path;
    std::string expectedBlake3;
};

struct RealReplayInput
{
    std::string collectionDatasetId;
    std::filesystem::path replayPath;
    std::string expectedReplayBlake3;
    std::string expectedPresenterRasterBlake3;
};

struct CalibrationPipelineInput
{
    std::vector<CodecSequenceInput> codecSequences;
    RealReplayInput realReplay;
    std::uint64_t maximumCodecFileBytes = 128ULL * 1024 * 1024;
    std::uint64_t maximumTotalCodecBytes = 512ULL * 1024 * 1024;
    std::uint64_t maximumReplayBytes = 128ULL * 1024 * 1024;
    std::uint32_t maximumReplayFrames = 128;
};

struct CalibrationArtifactEvidence
{
    std::string kind;
    std::string runId;
    std::uint64_t bytes = 0;
    std::string blake3;
};

struct CalibrationPipelineReport
{
    CalibrationReport calibration;
    std::vector<CalibrationArtifactEvidence> artifacts;
    std::string presenterRasterBlake3;
    std::string realCollectionDatasetId;
    std::string realReplayDatasetId;
    std::string realReplayRunId;
    std::uint64_t maximumCodecFileBytes = 0;
    std::uint64_t maximumTotalCodecBytes = 0;
    std::uint64_t maximumReplayBytes = 0;
    std::uint32_t maximumReplayFrames = 0;
    std::string canonicalJson;
};

// Builds the fixed provider-generic Step-07 simulator sweep, validates and
// decodes the explicitly sealed Step-06 Gray8 sequences, then fully validates
// the known-Presenter LF4 Replay before adding it as External truth. Input file
// hashes are checked before any observation becomes report evidence. Failure
// leaves output unchanged.
[[nodiscard]] bool BuildCalibrationEvidence(const CalibrationPipelineInput& input,
    CalibrationPipelineReport& output, std::string& error) noexcept;

} // namespace pbremotevisualmetriccalibration
