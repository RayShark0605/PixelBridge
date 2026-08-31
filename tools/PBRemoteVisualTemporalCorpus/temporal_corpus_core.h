#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pbremotevisualtemporalcorpus
{

inline constexpr char kTemporalCorpusSchema[] = "PixelBridge.RemoteVisualTemporalCorpus.2";
inline constexpr std::uint32_t kTemporalCorpusVersion = 2;
inline constexpr std::size_t kTemporalCorpusDigestBytes = 32;

struct TemporalCorpusReport
{
    std::uint32_t eventCount = 0;
    std::uint32_t admittedTransportBlocks = 0;
    std::uint32_t suppressedDuplicateEvents = 0;
    std::uint32_t suppressedReorderedEvents = 0;
    std::uint32_t duplicateRefinementRecoveries = 0;
    std::uint32_t wrongIdentityAcceptedTransportBlocks = 0;
    std::uint32_t wrongIdentityDiagnosticCandidates = 0;
    bool productionAdmissionSafe = false;
    bool expectationsMatched = false;
    std::array<std::byte, kTemporalCorpusDigestBytes> payloadBlake3{};
    std::string canonicalJson;
};

// Builds a bounded deterministic sequence corpus with production LF4 decode,
// QC-LDPC/Transport evaluation and the production application identity,
// duplicate-refinement and stall trackers. The corpus covers duplicate, gap,
// reorder, CaptureEpoch reset, duplicate refinement after an erasure, and a
// valid-CRC wrong-identity frame. No expected payload enters decoder inputs.
// On failure, output is unchanged.
[[nodiscard]] bool BuildDefaultTemporalCorpus(TemporalCorpusReport& output, std::string& error);

} // namespace pbremotevisualtemporalcorpus
