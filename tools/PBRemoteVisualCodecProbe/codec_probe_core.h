#pragma once

#include "receiver_evidence.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pbremotevisualcodecprobe
{

inline constexpr char kCodecSourceSchema[] = "PixelBridge.RemoteVisualCodecSource.1";
inline constexpr char kCodecEvaluationSchema[] = "PixelBridge.RemoteVisualCodecFrameEvaluation.2";
inline constexpr std::uint32_t kCodecSourceVersion = 1;
inline constexpr std::uint32_t kCodecEvaluationVersion = 2;
inline constexpr std::uint32_t kSourceWidth = 1920;
inline constexpr std::uint32_t kSourceHeight = 1080;
inline constexpr std::uint32_t kSourceFrameCount = 3;
inline constexpr std::uint32_t kMaximumEvaluationFrames = 16;
inline constexpr std::size_t kBgraFrameBytes = static_cast<std::size_t>(kSourceWidth) * kSourceHeight * 4;
inline constexpr std::size_t kGrayFrameBytes = static_cast<std::size_t>(kSourceWidth) * kSourceHeight;

struct CodecSourceSequence
{
    std::vector<std::byte> bgraFrames;
    std::string canonicalManifestJson;
};

struct CodecFrameSummary
{
    std::uint32_t index = 0;
    std::string classification;
    std::string erasure;
    bool bootstrapAccepted = false;
    std::uint64_t sessionTag = 0;
    std::uint64_t frameSequence = 0;
    // Production Transport admission and diagnostic truth scoring are kept
    // separate. Production does not infer SegmentOrdinal from FrameSequence.
    std::uint32_t acceptedTransportBlocks = 0;
    std::uint32_t fecFailures = 0;
    std::uint32_t crcFailures = 0;
    std::uint32_t identityFailures = 0;
    std::uint32_t diagnosticAcceptedTransportBlocks = 0;
    std::uint32_t diagnosticIdentityFailures = 0;
    std::uint32_t diagnosticFalseCandidates = 0;
    // Sender-truth mismatch among blocks accepted by the production Transport
    // boundary. This is distinct from CRC-valid diagnostic candidates that
    // production rejects on SessionTag identity.
    std::uint32_t falseAcceptedCodewords = 0;
};

struct CodecSequenceEvaluation
{
    std::vector<CodecFrameSummary> frames;
    pbremotevisualreceiverevidence::ReceiverEvidenceSummary receiverEvidence;
    bool truthBoundaryValid = false;
    bool allFramesVerified = false;
    std::string canonicalJson;
};

enum class CodecAdversarialFrameKind : std::uint8_t
{
    TransportErasure,
    ValidCrcWrongIdentity
};

// Produces three deterministic production LF4 BGRA frames. The manifest seals
// exact frame order, profile/session identity and raw byte hashes. No codec or
// provider behavior is simulated in this function.
[[nodiscard]] bool BuildCanonicalSourceSequence(CodecSourceSequence& output, std::string& error);

// Produces one Gray8 frame with a valid LF4 Bootstrap but adversarial data.
// TransportErasure carries no valid QC-LDPC codeword. ValidCrcWrongIdentity
// carries four valid Transport records whose identity intentionally disagrees
// with the Bootstrap. Production rejects all four at the identity gate, while
// the diagnostic truth oracle intentionally counts them as CRC-valid nontruth
// candidates. Neither variant changes production acceptance rules.
[[nodiscard]] bool BuildAdversarialGrayFrame(CodecAdversarialFrameKind kind,
    std::vector<std::byte>& output, std::string& error);

// Evaluates one to 16 tightly packed 1920x1080 Gray8 frames through the
// production LF4 demod, QC-LDPC and Transport boundary. The optional ordinal
// identifies the first sender-truth fixture Segment for post-admission corpus
// scoring only; production acceptance never infers it from FrameSequence.
// Expected payload bytes are never accepted as an argument. A failure returns
// no partial evaluation.
[[nodiscard]] bool EvaluateGray8Sequence(std::span<const std::byte> frames,
    CodecSequenceEvaluation& output, std::string& error,
    std::uint64_t firstExpectedSegmentOrdinal = 0);

} // namespace pbremotevisualcodecprobe
