#pragma once

#include "pbrealcapturereplay/replay_v2.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace pbremotevisualreplayinspector
{

inline constexpr char kReplayInspectionSchema[] = "PixelBridge.RemoteVisualReplayInspection.1";
inline constexpr std::uint32_t kReplayInspectionVersion = 1;

struct ReplayInspection
{
    std::uint32_t captureFrames = 0;
    std::uint32_t recordedDemodObservations = 0;
    std::uint32_t bootstrapAcceptedFrames = 0;
    std::uint32_t modulationAcceptedFrames = 0;
    std::uint32_t transportAcceptedFrames = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    std::string canonicalJson;
};

// Opens and fully validates the sealed Replay v2 before decoding any record.
// Every capture is decoded independently through the existing CPU/reference
// modulation and production Transport trust boundary. Receiver-only input has
// no sender truth, so false-accept counts are serialized as null rather than 0.
// Failure leaves output unchanged and never creates an output artifact.
[[nodiscard]] bool InspectReplay(const std::filesystem::path& replayPath,
    const pbrealcapturereplay::ReplayV2Limits& limits, ReplayInspection& output,
    std::string& error) noexcept;

} // namespace pbremotevisualreplayinspector
