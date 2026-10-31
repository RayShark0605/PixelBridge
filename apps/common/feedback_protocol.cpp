#include "feedback_protocol.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>
#include <utility>

namespace pbapp
{
FeedbackStatus BuildFeedbackIdentity(const pbprotocol::SessionDescriptor& session, const pbprotocol::FinalManifest& manifest,
    const std::uint8_t layoutVersion, const pbprotocol::ReceiverResourcePolicy& policy, FeedbackIdentity& output) noexcept
{
    try
    {
        if (session.segmentCount > maximumFeedbackSegmentCount || session.originalFileSize > maximumFeedbackFileBytes)
        {
            return {FeedbackError::LimitExceeded, "Session exceeds auxiliary feedback limits"};
        }
        const auto sessionSize = pbprotocol::GetSerializedSize(session);
        const auto manifestSize = pbprotocol::GetSerializedSize(manifest);
        if (!sessionSize || sessionSize.Value() > maximumFeedbackSnapshotBytes || manifestSize == 0 || manifestSize > maximumFeedbackSnapshotBytes)
        {
            return {FeedbackError::InvalidFormat, "Cannot canonicalize feedback identity"};
        }
        std::vector<std::byte> sessionBytes(sessionSize.Value());
        std::vector<std::byte> manifestBytes(manifestSize);
        if (!pbprotocol::SerializeSessionDescriptor(session, policy, sessionBytes) ||
            !pbprotocol::SerializeFinalManifest(manifest, session, policy, manifestBytes))
        {
            return {FeedbackError::InvalidFormat, "Cannot serialize canonical feedback identity"};
        }
        FeedbackIdentity identity;
        identity.sessionId = session.sessionId;
        identity.profileId = session.sessionVisualProfileId;
        identity.layoutVersion = layoutVersion;
        identity.fileBytes = session.originalFileSize;
        identity.segmentCount = static_cast<std::uint32_t>(session.segmentCount);
        identity.canonicalSessionDigest = pbprotocol::ComputeBlake3Digest(sessionBytes);
        identity.canonicalManifestDigest = pbprotocol::ComputeBlake3Digest(manifestBytes);
        identity.wholeFileDigest = manifest.wholeFileDigest;
        output = identity;
        return {};
    }
    catch (...)
    {
        return {FeedbackError::AllocationFailure, "Cannot allocate bounded feedback identity"};
    }
}

namespace
{

constexpr std::array<std::byte, 4> feedbackMagic{std::byte{0x50}, std::byte{0x42}, std::byte{0x46}, std::byte{0x42}};
constexpr std::uint16_t feedbackVersion = 1;
// Auxiliary v1 byte contract: prefix 0..23, scalar fields 24..63,
// Session/writer/epoch 64..111, three digests 112..207, bitmap length/reserved
// 208..215, then exactly ceil(SegmentCount/8) bytes and the final CRC32C.
// This is not a visual ControlRecord or a PBJR recovery record.
constexpr std::uint16_t feedbackHeaderBytes = 216;
constexpr std::size_t feedbackCrcBytes = 4;
constexpr std::size_t maximumFeedbackBitmapBytes = maximumFeedbackSegmentCount / 8U;
constexpr std::uint32_t identityBoundFlag = 1;
constexpr std::uint32_t observedFrameFlag = 2;
constexpr std::uint32_t wholeDigestFlag = 4;
constexpr std::uint32_t finalPublishFlag = 8;
constexpr std::uint32_t finalReopenFlag = 16;
constexpr std::uint32_t knownFeedbackFlags = 31;

FeedbackStatus Failure(const FeedbackError error, const char* const message) noexcept
{
    return {error, message, 0};
}

bool IsZero(const std::span<const std::byte> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](const std::byte value)
    {
        return value == std::byte{0};
    });
}

std::uint32_t GetFlags(const FeedbackSnapshot& snapshot) noexcept
{
    return (snapshot.identityBound ? identityBoundFlag : 0U) |
        (snapshot.observedFrameAvailable ? observedFrameFlag : 0U) |
        (snapshot.wholeDigestVerified ? wholeDigestFlag : 0U) |
        (snapshot.finalPublishSucceeded ? finalPublishFlag : 0U) |
        (snapshot.finalReopenVerified ? finalReopenFlag : 0U);
}

}

std::uint32_t GetFeedbackCompletedSegmentCount(const FeedbackSnapshot& snapshot) noexcept
{
    if (snapshot.identity.segmentCount > maximumFeedbackSegmentCount ||
        snapshot.durableCompleted.size() != (snapshot.identity.segmentCount + 7U) / 8U)
    {
        return 0;
    }
    std::uint32_t count = 0;
    for (const std::byte value : snapshot.durableCompleted)
    {
        count += static_cast<std::uint32_t>(std::popcount(std::to_integer<unsigned int>(value)));
    }
    return count;
}

FeedbackStatus ValidateFeedbackSnapshot(const FeedbackSnapshot& snapshot) noexcept
{
    if (snapshot.identity.segmentCount > maximumFeedbackSegmentCount || snapshot.identity.fileBytes > maximumFeedbackFileBytes ||
        snapshot.durableCompleted.size() > maximumFeedbackBitmapBytes)
    {
        return Failure(FeedbackError::LimitExceeded, "Feedback dimensions exceed limits");
    }
    if (snapshot.durableCompleted.size() != (snapshot.identity.segmentCount + 7U) / 8U ||
        (snapshot.identity.fileBytes == 0) != (snapshot.identity.segmentCount == 0))
    {
        return Failure(FeedbackError::InconsistentState, "Feedback segment dimensions are inconsistent");
    }
    if (snapshot.snapshotSequence == 0 || IsZero(snapshot.receiverId) || IsZero(snapshot.receiverEpoch))
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback writer identity or sequence is empty");
    }
    if (snapshot.phase < FeedbackPhase::Waiting || snapshot.phase > FeedbackPhase::Failed)
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback phase is unsupported");
    }
    const std::uint32_t remainder = snapshot.identity.segmentCount % 8U;
    if (remainder != 0 && (std::to_integer<std::uint32_t>(snapshot.durableCompleted.back()) & ~((1U << remainder) - 1U)) != 0)
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback bitmap has nonzero unused bits");
    }
    if (!snapshot.observedFrameAvailable && snapshot.observedFrameSequence != 0)
    {
        return Failure(FeedbackError::InconsistentState, "Feedback has an unavailable frame value");
    }
    if (snapshot.identityBound && (snapshot.identity.profileId == 0 || snapshot.identity.layoutVersion == 0))
    {
        return Failure(FeedbackError::InconsistentState, "Feedback bound visual identity is empty");
    }
    if (!snapshot.identityBound && (!IsZero(snapshot.durableCompleted) || snapshot.phase == FeedbackPhase::Receiving ||
        snapshot.phase == FeedbackPhase::Verifying || snapshot.phase == FeedbackPhase::Completed))
    {
        return Failure(FeedbackError::InconsistentState, "Unbound feedback cannot acknowledge recovery");
    }
    if (snapshot.phase == FeedbackPhase::Completed)
    {
        if (!snapshot.identityBound || !snapshot.wholeDigestVerified || !snapshot.finalPublishSucceeded || !snapshot.finalReopenVerified ||
            GetFeedbackCompletedSegmentCount(snapshot) != snapshot.identity.segmentCount)
        {
            return Failure(FeedbackError::InconsistentState, "Feedback completion lacks durable final evidence");
        }
    }
    else if (snapshot.wholeDigestVerified || snapshot.finalPublishSucceeded || snapshot.finalReopenVerified ||
        snapshot.reportedCompletionMilliseconds != 0)
    {
        return Failure(FeedbackError::InconsistentState, "Nonterminal feedback claims final verification");
    }
    return {};
}

FeedbackStatus SerializeFeedbackSnapshot(const FeedbackSnapshot& snapshot, std::vector<std::byte>& output) noexcept
{
    const FeedbackStatus valid = ValidateFeedbackSnapshot(snapshot);
    if (!valid)
    {
        return valid;
    }
    try
    {
        const std::size_t totalBytes = feedbackHeaderBytes + snapshot.durableCompleted.size() + feedbackCrcBytes;
        std::vector<std::byte> encoded(totalBytes);
        pbprotocol::ByteWriter writer(encoded);
        const bool written = writer.WriteFixedBytes(feedbackMagic) && writer.WriteUint16(feedbackVersion) &&
            writer.WriteUint16(feedbackHeaderBytes) && writer.WriteUint32(static_cast<std::uint32_t>(totalBytes)) &&
            writer.WriteUint32(GetFlags(snapshot)) && writer.WriteUint8(static_cast<std::uint8_t>(snapshot.phase)) &&
            writer.WriteUint8(snapshot.identity.layoutVersion) && writer.WriteUint16(0) && writer.WriteUint32(snapshot.identity.segmentCount) &&
            writer.WriteUint64(snapshot.identity.fileBytes) && writer.WriteUint64(snapshot.identity.profileId) &&
            writer.WriteUint64(snapshot.snapshotSequence) && writer.WriteUint64(snapshot.observedFrameSequence) &&
            writer.WriteUint64(snapshot.reportedCompletionMilliseconds) && writer.WriteFixedBytes(snapshot.identity.sessionId.bytes) &&
            writer.WriteFixedBytes(snapshot.receiverId) && writer.WriteFixedBytes(snapshot.receiverEpoch) &&
            writer.WriteFixedBytes(snapshot.identity.canonicalSessionDigest) && writer.WriteFixedBytes(snapshot.identity.canonicalManifestDigest) &&
            writer.WriteFixedBytes(snapshot.identity.wholeFileDigest.bytes) &&
            writer.WriteUint32(static_cast<std::uint32_t>(snapshot.durableCompleted.size())) && writer.WriteUint32(0);
        if (!written || writer.Position() != feedbackHeaderBytes || !writer.WriteBytes(snapshot.durableCompleted))
        {
            return Failure(FeedbackError::InvalidFormat, "Feedback serialization failed");
        }
        const std::uint32_t checksum = pbprotocol::ComputeCrc32c(writer.WrittenBytes());
        if (!writer.WriteUint32(checksum) || writer.Position() != totalBytes)
        {
            return Failure(FeedbackError::InvalidFormat, "Feedback checksum serialization failed");
        }
        output.swap(encoded);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback allocation failed");
    }
    catch (const std::length_error&)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback allocation length failed");
    }
}

FeedbackStatus ParseFeedbackSnapshot(const std::span<const std::byte> input, FeedbackSnapshot& output) noexcept
{
    if (input.size() < feedbackHeaderBytes + feedbackCrcBytes || input.size() > maximumFeedbackSnapshotBytes)
    {
        return Failure(FeedbackError::LimitExceeded, "Feedback file length is outside limits");
    }
    pbprotocol::ByteReader reader(input.first(feedbackHeaderBytes));
    const auto magic = reader.ReadFixedBytes<4>();
    const auto version = reader.ReadUint16();
    const auto headerBytes = reader.ReadUint16();
    const auto totalBytes = reader.ReadUint32();
    const auto flags = reader.ReadUint32();
    const auto phase = reader.ReadUint8();
    const auto layout = reader.ReadUint8();
    const auto reserved = reader.ReadUint16();
    const auto segmentCount = reader.ReadUint32();
    const auto fileBytes = reader.ReadUint64();
    const auto profileId = reader.ReadUint64();
    const auto sequence = reader.ReadUint64();
    const auto frameSequence = reader.ReadUint64();
    const auto completionMilliseconds = reader.ReadUint64();
    const auto sessionId = reader.ReadFixedBytes<16>();
    const auto receiverId = reader.ReadFixedBytes<16>();
    const auto receiverEpoch = reader.ReadFixedBytes<16>();
    const auto sessionDigest = reader.ReadFixedBytes<32>();
    const auto manifestDigest = reader.ReadFixedBytes<32>();
    const auto wholeDigest = reader.ReadFixedBytes<32>();
    const auto bitmapBytes = reader.ReadUint32();
    const auto reservedTail = reader.ReadUint32();
    if (!magic || !version || !headerBytes || !totalBytes || !flags || !phase || !layout || !reserved || !segmentCount ||
        !fileBytes || !profileId || !sequence || !frameSequence || !completionMilliseconds || !sessionId || !receiverId ||
        !receiverEpoch || !sessionDigest || !manifestDigest || !wholeDigest || !bitmapBytes || !reservedTail)
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback header is incomplete");
    }
    if (magic.Value() != feedbackMagic || headerBytes.Value() != feedbackHeaderBytes || totalBytes.Value() != input.size() ||
        (flags.Value() & ~knownFeedbackFlags) != 0 || reserved.Value() != 0 || reservedTail.Value() != 0)
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback header is noncanonical");
    }
    if (version.Value() != feedbackVersion)
    {
        return Failure(FeedbackError::UnsupportedVersion, "Feedback version is unsupported");
    }
    if (segmentCount.Value() > maximumFeedbackSegmentCount || fileBytes.Value() > maximumFeedbackFileBytes ||
        bitmapBytes.Value() > maximumFeedbackBitmapBytes || bitmapBytes.Value() != (segmentCount.Value() + 7U) / 8U ||
        bitmapBytes.Value() != input.size() - feedbackHeaderBytes - feedbackCrcBytes)
    {
        return Failure(FeedbackError::LimitExceeded, "Feedback bitmap length is inconsistent");
    }
    pbprotocol::ByteReader checksumReader(input.last(feedbackCrcBytes));
    const auto checksum = checksumReader.ReadUint32();
    if (!checksum || checksum.Value() != pbprotocol::ComputeCrc32c(input.first(input.size() - feedbackCrcBytes)))
    {
        return Failure(FeedbackError::IntegrityMismatch, "Feedback checksum mismatch");
    }
    try
    {
        FeedbackSnapshot parsed;
        parsed.identity.sessionId.bytes = sessionId.Value();
        parsed.identity.profileId = profileId.Value();
        parsed.identity.layoutVersion = layout.Value();
        parsed.identity.fileBytes = fileBytes.Value();
        parsed.identity.segmentCount = segmentCount.Value();
        parsed.identity.canonicalSessionDigest = sessionDigest.Value();
        parsed.identity.canonicalManifestDigest = manifestDigest.Value();
        parsed.identity.wholeFileDigest.bytes = wholeDigest.Value();
        parsed.receiverId = receiverId.Value();
        parsed.receiverEpoch = receiverEpoch.Value();
        parsed.snapshotSequence = sequence.Value();
        parsed.observedFrameSequence = frameSequence.Value();
        parsed.reportedCompletionMilliseconds = completionMilliseconds.Value();
        parsed.phase = static_cast<FeedbackPhase>(phase.Value());
        parsed.identityBound = (flags.Value() & identityBoundFlag) != 0;
        parsed.observedFrameAvailable = (flags.Value() & observedFrameFlag) != 0;
        parsed.wholeDigestVerified = (flags.Value() & wholeDigestFlag) != 0;
        parsed.finalPublishSucceeded = (flags.Value() & finalPublishFlag) != 0;
        parsed.finalReopenVerified = (flags.Value() & finalReopenFlag) != 0;
        const auto bitmap = input.subspan(feedbackHeaderBytes, bitmapBytes.Value());
        parsed.durableCompleted.assign(bitmap.begin(), bitmap.end());
        const FeedbackStatus valid = ValidateFeedbackSnapshot(parsed);
        if (!valid)
        {
            return valid;
        }
        output = std::move(parsed);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback parsing allocation failed");
    }
    catch (const std::length_error&)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback parsing allocation length failed");
    }
}

}
