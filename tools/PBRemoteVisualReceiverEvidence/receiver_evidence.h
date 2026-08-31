#pragma once

#include "pbdesktoplevels/reference_channel.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pbremotevisualreceiverevidence
{

inline constexpr std::uint32_t kReceiverEvidenceOuterBlockBytes =
    static_cast<std::uint32_t>(pbdesktoplevels::kPayloadBytes);
inline constexpr std::uint64_t kReceiverEvidenceSegmentBytes =
    4ULL * kReceiverEvidenceOuterBlockBytes;
inline constexpr std::uint32_t kMaximumReceiverEvidenceSegments = 64;
inline constexpr std::uint32_t kMaximumReceiverEvidenceInputBlocks =
    kMaximumReceiverEvidenceSegments * 8;

enum class WholeFileDigestDisposition : std::uint8_t
{
    NotReady,
    Pass,
    Mismatch,
    ReceiverRejected
};

struct ReceiverEvidenceSummary
{
    std::uint32_t configuredSegments = 0;
    std::uint32_t inputTransportBlocks = 0;
    std::uint32_t parsedTransportBlocks = 0;
    std::uint32_t uniqueOuterSymbols = 0;
    std::uint32_t identicalDuplicateOuterSymbols = 0;
    std::uint32_t recoveryReadyOuterSymbols = 0;
    std::uint32_t alreadyCompletedOuterSymbols = 0;
    std::uint32_t receiverRejections = 0;
    std::uint32_t outerConflictRejections = 0;
    std::uint64_t resourcePolicyRejections = 0;
    std::uint32_t verifiedSegments = 0;
    std::uint64_t verifiedRawBytes = 0;
    bool finalizationPrepared = false;
    WholeFileDigestDisposition wholeFileDigestDisposition = WholeFileDigestDisposition::NotReady;
    std::string expectedWholeFileBlake3;
    std::string observedWholeFileBlake3;

    [[nodiscard]] bool IsSafe() const noexcept;
};

// Fixed, tool-only Session identity for deterministic corpus reconstruction.
// It is not a production sender Session and never changes a wire profile.
[[nodiscard]] const pbprotocol::SessionId& GetReceiverEvidenceSessionId() noexcept;
[[nodiscard]] pbprotocol::SessionTag GetReceiverEvidenceSessionTag() noexcept;

[[nodiscard]] bool MakeReceiverEvidenceBootstrapRecord(std::uint64_t frameSequence,
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& output, std::string& error);

// Reconstructs the sender-side canonical Transport truth for one diagnostic
// LF4 slot. This is for post-admission scoring and descriptor construction;
// callers must never pass it into demodulation or Inner-FEC decoding.
[[nodiscard]] bool MakeReceiverEvidenceExpectedTransportBlock(std::uint64_t segmentOrdinal,
    std::uint32_t slot, pbdesktoplevels::AcceptedTransportBlock& output, std::string& error);

// Post-admission corpus oracle: compares blocks accepted by the production
// Transport boundary with the sender fixture for one explicitly known source
// Segment. This does not add a FrameSequence-to-SegmentOrdinal production gate.
[[nodiscard]] std::uint32_t CountProductionTruthMismatches(std::uint64_t expectedSegmentOrdinal,
    std::span<const pbdesktoplevels::AcceptedTransportBlock> acceptedBlocks);

// Replays only Transport blocks that have already passed production
// QC-LDPC/padding/CRC/Bootstrap-SessionTag admission. Sender truth is used to
// construct ordinary Control descriptors and to verify recovered storage; it
// is never passed to modulation, soft demodulation, or Inner-FEC decoding.
// The bounded in-memory storage sink models the exact write/flush-before-commit
// contract but deliberately does not claim PBStorage publication.
[[nodiscard]] bool EvaluateReceiverEvidence(std::uint32_t segmentCount,
    std::span<const pbdesktoplevels::AcceptedTransportBlock> acceptedBlocks,
    ReceiverEvidenceSummary& output, std::string& error);

[[nodiscard]] const char* GetWholeFileDigestDispositionName(WholeFileDigestDisposition disposition) noexcept;

} // namespace pbremotevisualreceiverevidence
