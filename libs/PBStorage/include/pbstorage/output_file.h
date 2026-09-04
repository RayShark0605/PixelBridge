#pragma once

#include "pbprotocol/protocol_types.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace pbstorage
{

enum class StorageErrorCode : std::uint8_t
{
    None,
    InvalidArgument,
    InvalidDirectory,
    TargetExists,
    ResourceLimit,
    NativeFailure,
    DigestMismatch,
    AlreadyPublished,
    OutOfMemory,
    InternalError,
    ResumeMismatch,
    OverlappingRange,
    IncompleteFile
};

enum class StorageStage : std::uint8_t
{
    None,
    Configuration,
    Directory,
    Reservation,
    Preallocation,
    Write,
    Flush,
    Digest,
    Publish,
    Cleanup,
    Resume
};

struct StorageStatus
{
    StorageErrorCode code = StorageErrorCode::None;
    StorageStage stage = StorageStage::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == StorageErrorCode::None;
    }

    [[nodiscard]] static StorageStatus Success() noexcept
    {
        return {};
    }

    [[nodiscard]] static StorageStatus Failure(StorageErrorCode code, StorageStage stage,
        std::int32_t nativeError = 0) noexcept;

    bool operator==(const StorageStatus&) const = default;
};

struct OutputFileConfig
{
    std::wstring outputDirectory;
    pbprotocol::SessionTag sessionTag{};
    std::uint64_t fileBytes = 0;
    std::uint64_t maximumFileBytes = 0;
    // Empty retains the diagnostic PixelBridge-<SessionTag>.bin name. Formal
    // product sessions pass the already validated descriptor basename here.
    std::string originalFileNameUtf8;
};

struct OutputFileReservation
{
    // UTF-8 basename selected before .part creation and persisted by the
    // application resume journal. It is revalidated against the only legal
    // primary/collision names on every reopen.
    std::string finalFileNameUtf8;

    bool operator==(const OutputFileReservation&) const = default;
};

struct OutputFileSnapshot
{
    std::wstring finalPath;
    std::wstring partPath;
    std::uint64_t fileBytes = 0;
    std::uint64_t writtenBytes = 0;
    std::uint64_t verifiedBytes = 0;
    std::uint64_t availableBytesBeforeReservation = 0;
    std::uint64_t requestedAllocationBytes = 0;
    std::uint64_t actualAllocationBytes = 0;
    bool resumed = false;
    bool hasPendingWrite = false;
    bool published = false;
    bool recoveredPublished = false;
    // null = not observed in this object; false = attempted phase failed.
    // Recovered publication observes the final digest, not the prior rename.
    std::optional<bool> wholeFileDigestVerified;
    std::optional<bool> finalRenameSucceeded;
    std::optional<bool> finalReopenVerified;
    bool preallocationAttempted = false;
    bool preallocationFullyAllocated = false;
    bool fileSparse = false;
    bool fileCompressed = false;
    bool volumeSupportsSparseFiles = false;
    bool volumeSupportsCompression = false;
    bool volumeCompressed = false;
};

[[nodiscard]] const char* GetStorageErrorName(StorageErrorCode code) noexcept;
[[nodiscard]] const char* GetStorageStageName(StorageStage stage) noexcept;

// Owns or resumes a same-directory PixelBridge-<SessionTag>.part reservation.
// Verified Segment writes may arrive out of order but never overlap. A write is
// not counted as verified until FlushVerifiedSegment succeeds. Destruction
// preserves unpublished .part data for crash/process restart recovery.
class OutputFile
{
public:
    [[nodiscard]] static StorageStatus PlanReservation(const OutputFileConfig& config,
        OutputFileReservation& output) noexcept;
    [[nodiscard]] static StorageStatus Create(const OutputFileConfig& config,
        std::unique_ptr<OutputFile>& output) noexcept;
    [[nodiscard]] static StorageStatus CreateOrResume(const OutputFileConfig& config,
        std::unique_ptr<OutputFile>& output) noexcept;
    [[nodiscard]] static StorageStatus CreateOrResume(const OutputFileConfig& config,
        const OutputFileReservation& reservation,
        const std::optional<pbprotocol::WholeFileDigest>& publishIntent,
        std::unique_ptr<OutputFile>& output) noexcept;

    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    [[nodiscard]] StorageStatus Write(std::uint64_t rawOffset,
        std::span<const std::byte> rawBytes) noexcept;
    [[nodiscard]] StorageStatus WriteVerifiedSegment(std::uint64_t rawOffset,
        std::span<const std::byte> rawBytes) noexcept;
    [[nodiscard]] StorageStatus ReadRange(std::uint64_t rawOffset,
        std::span<std::byte> output) noexcept;
    // Marks a range verified only after the caller has re-read it and checked
    // the bound Segment RawDigest. No data is rewritten.
    [[nodiscard]] StorageStatus AdoptVerifiedSegment(std::uint64_t rawOffset,
        std::uint64_t rawSize) noexcept;
    [[nodiscard]] StorageStatus FlushVerifiedSegment() noexcept;
    [[nodiscard]] StorageStatus Checkpoint() noexcept;
    [[nodiscard]] StorageStatus Publish(const pbprotocol::WholeFileDigest& expectedDigest) noexcept;
    [[nodiscard]] StorageStatus Discard() noexcept;
    [[nodiscard]] OutputFileSnapshot GetSnapshot() const;

private:
    struct Implementation;
    [[nodiscard]] static StorageStatus CreateInternal(const OutputFileConfig& config,
        bool allowResume, const OutputFileReservation* reservation,
        const std::optional<pbprotocol::WholeFileDigest>& publishIntent,
        std::unique_ptr<OutputFile>& output) noexcept;
    explicit OutputFile(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbstorage
