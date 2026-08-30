#pragma once

#include "pbprotocol/protocol_types.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    InternalError
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
    Cleanup
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
};

struct OutputFileSnapshot
{
    std::wstring finalPath;
    std::wstring partPath;
    std::uint64_t fileBytes = 0;
    std::uint64_t writtenBytes = 0;
    bool published = false;
};

[[nodiscard]] const char* GetStorageErrorName(StorageErrorCode code) noexcept;
[[nodiscard]] const char* GetStorageStageName(StorageStage stage) noexcept;

// Owns a newly created same-directory .part reservation. It never overwrites
// an existing .part or final path. Publish first verifies the exact sequential
// BLAKE3 digest, then performs a write-through same-directory rename and
// re-verifies the final artifact. A failed final verification attempts to
// remove the just-renamed artifact and reports cleanup failure explicitly.
// Destruction removes only this instance's unpublished .part file; a
// successfully published final artifact is never deleted.
class OutputFile
{
public:
    [[nodiscard]] static StorageStatus Create(const OutputFileConfig& config,
        std::unique_ptr<OutputFile>& output) noexcept;

    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    [[nodiscard]] StorageStatus Write(std::uint64_t rawOffset,
        std::span<const std::byte> rawBytes) noexcept;
    [[nodiscard]] StorageStatus Publish(const pbprotocol::WholeFileDigest& expectedDigest) noexcept;
    [[nodiscard]] OutputFileSnapshot GetSnapshot() const;

private:
    struct Implementation;
    explicit OutputFile(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbstorage
