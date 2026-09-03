#pragma once

#include "pbprotocol/resume_state.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pbapp
{

struct DecoderResumeStoreStatus
{
    bool success = true;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }

    [[nodiscard]] static DecoderResumeStoreStatus Failure(std::string message)
    {
        return {false, std::move(message)};
    }
};

struct DecoderResumeAcceptedBlock
{
    std::uint64_t segmentOrdinal = 0;
    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    std::vector<std::byte> paddedPayload;

    bool operator==(const DecoderResumeAcceptedBlock&) const = default;
};

struct DecoderResumeLoadedState
{
    std::vector<std::vector<std::byte>> segmentControlRecords;
    std::vector<std::byte> manifestControlRecord;
    std::vector<pbprotocol::ResumeCompletedSegmentRecord> completedSegments;
    std::vector<DecoderResumeAcceptedBlock> activeBlocks;
    std::optional<std::string> outputReservationFileNameUtf8;
    std::optional<pbprotocol::WholeFileDigest> publishIntent;
    bool resumed = false;
    bool hadTruncatedTail = false;
    std::uint64_t generation = 0;
};

// Receiver-local append journal. New accepted blocks are batched and flushed
// at explicit checkpoints; completed records are flushed only after .part data
// and then compacted atomically. The final incomplete record is the only
// tolerated corruption shape.
class DecoderResumeStore
{
public:
    [[nodiscard]] static DecoderResumeStoreStatus Open(const std::filesystem::path& outputDirectory,
        pbprotocol::SessionTag sessionTag, std::span<const std::byte> sessionControlRecord,
        const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
        std::unique_ptr<DecoderResumeStore>& output, DecoderResumeLoadedState& loaded) noexcept;

    DecoderResumeStore(const DecoderResumeStore&) = delete;
    DecoderResumeStore& operator=(const DecoderResumeStore&) = delete;
    ~DecoderResumeStore();

    [[nodiscard]] DecoderResumeStoreStatus RecordSegmentControl(std::span<const std::byte> controlRecord) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RecordManifestControl(std::span<const std::byte> controlRecord) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RecordOutputReservation(std::string finalFileNameUtf8) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RecordAcceptedBlock(const DecoderResumeAcceptedBlock& block) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus Checkpoint() noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RecordCompletedSegment(
        const pbprotocol::ResumeCompletedSegmentRecord& completedRecord) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RecordPublishIntent(
        const pbprotocol::WholeFileDigest& wholeFileDigest) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus RemoveAfterPublish() noexcept;

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept;
    [[nodiscard]] std::uint64_t GetGeneration() const noexcept;
    [[nodiscard]] std::uint64_t GetFileBytes() const noexcept;
    [[nodiscard]] std::size_t GetActiveBlockCount() const noexcept;
    [[nodiscard]] std::size_t GetPendingBlockCount() const noexcept;
    [[nodiscard]] bool WasResumed() const noexcept;
    [[nodiscard]] bool HadTruncatedTail() const noexcept;

private:
    struct Implementation;
    explicit DecoderResumeStore(std::unique_ptr<Implementation> implementation) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus AppendRecord(std::uint16_t recordType,
        std::span<const std::byte> payload, bool flush) noexcept;
    [[nodiscard]] DecoderResumeStoreStatus Compact() noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbapp
