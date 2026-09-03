#pragma once

#include "pbreceiver/receiver_result.h"

#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/orphan_transport_block_cache.h"
#include "pbprotocol/output_reservation.h"
#include "pbprotocol/resume_state.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pbreceiver
{

namespace detail
{
struct ReceiverIngressImplementation;
}

// Logical Data Block after upstream frame, Inner-FEC, and Transport CRC
// validation. This local API deliberately does not freeze the provisional
// serialized Transport header.
struct ReceivedTransportBlock
{
    pbprotocol::SessionTag sessionTag{};
    std::uint64_t segmentOrdinal = 0;
    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    std::span<const std::byte> paddedPayload;
};

struct ReceiverCompletedSegment
{
    ReceiverCompletedSegment(
        pbprotocol::BoundSegmentDescriptor boundDescriptor,
        std::vector<std::byte> recoveredEncodedBytes)
        : boundSegmentDescriptor(std::move(boundDescriptor)),
          encodedBytes(std::move(recoveredEncodedBytes))
    {
    }

    pbprotocol::BoundSegmentDescriptor boundSegmentDescriptor;
    std::vector<std::byte> encodedBytes;
};

// Move-only proof of raw bytes against the current immutable binding. Fresh
// recovery also verifies encoded bytes and bounded decompression; resumed
// stored bytes are rehashed directly, never misinterpreted as encoded bytes.
// Storage code can inspect but cannot mutate the bytes before exact commit.
class ReceiverVerifiedSegment
{
public:
    ReceiverVerifiedSegment(const ReceiverVerifiedSegment&) = delete;
    ReceiverVerifiedSegment& operator=(const ReceiverVerifiedSegment&) = delete;
    ReceiverVerifiedSegment(ReceiverVerifiedSegment&& other) noexcept;
    ReceiverVerifiedSegment& operator=(ReceiverVerifiedSegment&& other) noexcept;

    [[nodiscard]] const pbprotocol::BoundSegmentDescriptor&
    GetBoundSegmentDescriptor() const noexcept;
    [[nodiscard]] std::span<const std::byte> GetRawBytes() const noexcept;

private:
    friend class ReceiverIngress;

    ReceiverVerifiedSegment(
        pbprotocol::BoundSegmentDescriptor boundDescriptor,
        std::vector<std::byte> verifiedRawBytes) noexcept;

    pbprotocol::BoundSegmentDescriptor boundSegmentDescriptor_;
    std::vector<std::byte> rawBytes_;
    bool valid_ = true;
};

enum class ReceiverSegmentCommitDisposition : std::uint8_t
{
    Committed,
    AlreadyCommitted
};

enum class ReceiverDataDisposition : std::uint8_t
{
    CachedOrphan,
    AcceptedNeedMore,
    AlreadyCompleted,
    EncodedSegmentReady,
    // The descriptor is valid but all bounded decoder slots are occupied.
    // The block was not admitted by a new decoder and may be retried by a
    // later Carousel pass after another Segment is durably committed.
    DeferredResourceBusy
};

enum class ReceiverOuterSymbolAdmission : std::uint8_t
{
    NotApplicable,
    Unique,
    IdenticalDuplicate,
    RecoveryAlreadyReady,
    AlreadyCompleted,
    DeferredResourceBusy
};

struct ReceiverDataAdmission
{
    ReceiverDataDisposition disposition =
        ReceiverDataDisposition::AcceptedNeedMore;
    std::optional<ReceiverCompletedSegment> completedSegment;
    // Non-wire admission telemetry. Unique is emitted only when the bounded
    // orphan cache or bound Outer decoder retained a new OuterBlockId.
    ReceiverOuterSymbolAdmission outerSymbolAdmission = ReceiverOuterSymbolAdmission::NotApplicable;
    // Populated when a later bound-data observation drains pre-descriptor
    // orphan blocks after decoder capacity becomes available.
    std::vector<pbprotocol::OrphanTransportBlockEntry> replayedOrphanBlocks;
};

struct ReceiverControlAdmission
{
    pbprotocol::ControlRecordAdmission controlAdmission{};
    std::optional<pbprotocol::OutputReservationDecision>
        outputReservationDecision;
    // Unique pre-descriptor blocks that this SegmentDescriptor actually
    // replayed into a newly available bounded decoder. The application owns
    // these copies so it can checkpoint the same accepted equations only
    // after the descriptor has passed its resource and range validation.
    std::vector<pbprotocol::OrphanTransportBlockEntry> replayedOrphanBlocks;
    std::optional<ReceiverCompletedSegment> completedSegment;
};

struct ReceiverControlFragmentResult
{
    pbprotocol::ControlFragmentReceiveDisposition disposition =
        pbprotocol::ControlFragmentReceiveDisposition::Stored;
    std::optional<ReceiverControlAdmission> admission;
};

struct ReceiverResourceTelemetrySnapshot
{
    std::uint64_t totalResourcePolicyRejectedCount = 0;
    std::uint64_t controlRejectedByResourcePolicyCount = 0;
    std::size_t activeControlReassemblyCount = 0;
    std::size_t controlReassemblyBytesInUse = 0;
    std::uint64_t outerFecQuotaExceededCount = 0;
    std::uint64_t deferredResourceBusyCount = 0;
    std::uint64_t orphanAdmittedBlockCount = 0;
    std::uint64_t orphanDroppedByQuotaCount = 0;
    std::uint64_t orphanResourceExhaustedCount = 0;
    std::uint64_t orphanConflictRejectionCount = 0;
    std::size_t orphanCachedBlockCount = 0;
    std::size_t orphanCachedBytes = 0;
    std::uint64_t resumeQuotaRejectedCount = 0;
    std::uint64_t decompressionResourceRejectedCount = 0;
    std::uint64_t decompressionInputQuotaRejectedCount = 0;
    std::uint64_t decompressionOutputQuotaRejectedCount = 0;
    std::uint64_t decompressionWindowQuotaRejectedCount = 0;
    std::uint64_t decompressionAllocationFailureCount = 0;
    std::uint64_t outputReservationDeniedCount = 0;
    std::uint64_t outputReservationRequiresConfirmationCount = 0;
    std::uint64_t outputReservationAutoAcceptedCount = 0;
    std::size_t activeSessionCount = 0;
    std::uint64_t reservedDescriptorStateBytes = 0;
    std::uint64_t activeOuterFecDecoderCount = 0;
    std::uint64_t reservedOuterFecDecoderBytes = 0;
};

// Authoritative, single-owner receiver ingress for bounded Control/Data
// admission. Cross-thread callers must serialize access. Destruction and
// capture-epoch reset destroy active decoders before their shared manager.
class ReceiverIngress
{
public:
    [[nodiscard]] static ReceiverResult<ReceiverIngress> Create(
        pbprotocol::ReceiverResourcePolicy resourcePolicy,
        std::uint32_t expectedOuterBlockBytes);

    ReceiverIngress(const ReceiverIngress&) = delete;
    ReceiverIngress& operator=(const ReceiverIngress&) = delete;
    ReceiverIngress(ReceiverIngress&&) noexcept;
    ReceiverIngress& operator=(ReceiverIngress&&) noexcept;
    ~ReceiverIngress();

    [[nodiscard]] ReceiverResult<ReceiverControlAdmission>
    ReceiveControlRecord(std::span<const std::byte> recordBytes);
    [[nodiscard]] ReceiverResult<ReceiverControlFragmentResult>
    ReceiveControlFragment(
        std::span<const std::byte> fragmentBytes,
        std::uint64_t observationOrdinal);
    [[nodiscard]] ReceiverResult<ReceiverDataAdmission> ReceiveDataBlock(
        const ReceivedTransportBlock& transportBlock,
        std::optional<std::uint64_t> observationOrdinal = std::nullopt);

    [[nodiscard]] ReceiverResult<std::vector<std::byte>> ParseResumeState(
        std::span<const std::byte> recordBytes);
    [[nodiscard]] ReceiverResult<pbprotocol::OutputReservationDecision>
    EvaluateOutputReservation(std::uint64_t originalFileSize);
    // Applies this receiver's bounded decompression limits and verifies both
    // the encoded input digest and recovered raw digest before returning data.
    [[nodiscard]] ReceiverResult<std::vector<std::byte>> DecompressSegment(
        const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
        std::span<const std::byte> encodedBytes);
    [[nodiscard]] ReceiverResult<ReceiverVerifiedSegment>
    VerifyRecoveredSegment(ReceiverCompletedSegment&& completedSegment);
    // Callers validate metadata against the bound descriptor before reading
    // its bounded .part range. This rechecks the metadata and raw digest, but
    // does not mark completion or claim a new encoded/decompression check.
    [[nodiscard]] ReceiverResult<ReceiverVerifiedSegment>
    VerifyResumedStoredSegment(
        const pbprotocol::ResumeCompletedSegmentRecord& completedRecord,
        std::vector<std::byte>&& storedRawBytes);
    // Call only after the exact raw bytes have been written at RawOffset and
    // the caller's storage contract has completed its flush/close step.
    [[nodiscard]] ReceiverResult<ReceiverSegmentCommitDisposition>
    CommitStoredSegment(ReceiverVerifiedSegment&& verifiedSegment);
    [[nodiscard]] ReceiverResult<pbprotocol::FinalManifest> PrepareFinalization(
        pbprotocol::SessionTag sessionTag);

    [[nodiscard]] ReceiverResult<bool> RemoveSession(
        const pbprotocol::SessionId& sessionId);
    [[nodiscard]] ReceiverResult<bool> ResetCaptureEpoch(
        std::uint32_t expectedOuterBlockBytes);

    [[nodiscard]] ReceiverResourceTelemetrySnapshot GetTelemetry() const;

private:
    explicit ReceiverIngress(
        std::unique_ptr<detail::ReceiverIngressImplementation>
            implementation) noexcept;

    std::unique_ptr<detail::ReceiverIngressImplementation> implementation_;
};

} // namespace pbreceiver
