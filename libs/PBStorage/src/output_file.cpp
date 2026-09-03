#include "pbstorage/output_file.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace pbstorage
{
namespace
{

struct VerifiedRange
{
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

[[nodiscard]] StorageStatus LastError(const StorageStage stage) noexcept
{
    const DWORD error = GetLastError();
    return StorageStatus::Failure(StorageErrorCode::NativeFailure, stage,
        static_cast<std::int32_t>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error));
}

[[nodiscard]] bool PathExists(const std::wstring& path, DWORD& error) noexcept
{
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}

[[nodiscard]] StorageStatus RequireAbsent(const std::wstring& path) noexcept
{
    DWORD error = ERROR_SUCCESS;
    if (PathExists(path, error))
    {
        return StorageStatus::Failure(StorageErrorCode::TargetExists, StorageStage::Reservation, ERROR_FILE_EXISTS);
    }
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
        return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Reservation,
            static_cast<std::int32_t>(error));
    }
    return StorageStatus::Success();
}

[[nodiscard]] std::wstring MakeSessionTagText(const pbprotocol::SessionTag sessionTag)
{
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << sessionTag.value;
    return stream.str();
}

[[nodiscard]] std::wstring MakeDiagnosticFinalName(const pbprotocol::SessionTag sessionTag)
{
    return L"PixelBridge-" + MakeSessionTagText(sessionTag) + L".bin";
}

[[nodiscard]] std::wstring MakePartName(const pbprotocol::SessionTag sessionTag)
{
    return L"PixelBridge-" + MakeSessionTagText(sessionTag) + L".part";
}

[[nodiscard]] StorageStatus Utf8FileNameToWide(const std::string_view fileNameUtf8,
    std::wstring& output) noexcept
{
    const pbprotocol::ProtocolStatus validationStatus = pbprotocol::ValidateFileNameUtf8(fileNameUtf8);
    if (!validationStatus)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Configuration);
    }
    if (fileNameUtf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return StorageStatus::Failure(StorageErrorCode::ResourceLimit, StorageStage::Configuration);
    }
    const int inputBytes = static_cast<int>(fileNameUtf8.size());
    const int wideCharacters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        fileNameUtf8.data(), inputBytes, nullptr, 0);
    if (wideCharacters <= 0)
    {
        return LastError(StorageStage::Configuration);
    }
    try
    {
        output.resize(static_cast<std::size_t>(wideCharacters));
    }
    catch (const std::bad_alloc&)
    {
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Configuration);
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fileNameUtf8.data(), inputBytes,
        output.data(), wideCharacters) != wideCharacters)
    {
        output.clear();
        return LastError(StorageStage::Configuration);
    }
    return StorageStatus::Success();
}

[[nodiscard]] std::wstring MakeCollisionName(const std::wstring& originalName,
    const pbprotocol::SessionTag sessionTag)
{
    const std::filesystem::path originalPath(originalName);
    return originalPath.stem().wstring() + L" (PixelBridge-" + MakeSessionTagText(sessionTag) + L")" +
        originalPath.extension().wstring();
}

[[nodiscard]] StorageStatus SelectFinalPath(const OutputFileConfig& config,
    const std::filesystem::path& directory, std::wstring& output) noexcept
{
    try
    {
        if (config.originalFileNameUtf8.empty())
        {
            output = (directory / MakeDiagnosticFinalName(config.sessionTag)).wstring();
            return RequireAbsent(output);
        }

        std::wstring originalName;
        const StorageStatus conversionStatus = Utf8FileNameToWide(config.originalFileNameUtf8, originalName);
        if (!conversionStatus)
        {
            return conversionStatus;
        }
        const std::wstring primaryPath = (directory / originalName).wstring();
        DWORD primaryError = ERROR_SUCCESS;
        if (!PathExists(primaryPath, primaryError))
        {
            if (primaryError != ERROR_FILE_NOT_FOUND && primaryError != ERROR_PATH_NOT_FOUND)
            {
                return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Reservation,
                    static_cast<std::int32_t>(primaryError));
            }
            output = primaryPath;
            return StorageStatus::Success();
        }

        const std::wstring collisionName = MakeCollisionName(originalName, config.sessionTag);
        output = (directory / collisionName).wstring();
        return RequireAbsent(output);
    }
    catch (const std::bad_alloc&)
    {
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Reservation);
    }
    catch (...)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Directory);
    }
}

[[nodiscard]] StorageStatus GetFileLength(const HANDLE handle, std::uint64_t& output,
    const StorageStage stage) noexcept
{
    LARGE_INTEGER length{};
    if (GetFileSizeEx(handle, &length) == FALSE)
    {
        return LastError(stage);
    }
    if (length.QuadPart < 0)
    {
        return StorageStatus::Failure(StorageErrorCode::InternalError, stage);
    }
    output = static_cast<std::uint64_t>(length.QuadPart);
    return StorageStatus::Success();
}

[[nodiscard]] StorageStatus HashFile(const std::wstring& path,
    std::array<std::byte, pbprotocol::kDigestBytes>& output,
    std::uint64_t& fileBytes) noexcept
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return LastError(StorageStage::Digest);
    }
    StorageStatus status = GetFileLength(handle, fileBytes, StorageStage::Digest);
    if (!status)
    {
        CloseHandle(handle);
        return status;
    }

    pbprotocol::Blake3Hasher hasher;
    std::vector<std::byte> buffer;
    try
    {
        buffer.resize(1024U * 1024U);
    }
    catch (const std::bad_alloc&)
    {
        CloseHandle(handle);
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Digest);
    }
    for (;;)
    {
        DWORD readBytes = 0;
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) == FALSE)
        {
            status = LastError(StorageStage::Digest);
            break;
        }
        if (readBytes == 0)
        {
            output = hasher.Finalize();
            break;
        }
        hasher.Update(std::span(buffer).first(readBytes));
    }
    if (CloseHandle(handle) == FALSE && status)
    {
        status = LastError(StorageStage::Digest);
    }
    return status;
}

[[nodiscard]] StorageStatus ReserveNewFile(const HANDLE handle,
    const std::wstring& outputDirectory, const std::uint64_t fileBytes) noexcept
{
    ULARGE_INTEGER availableBytes{};
    if (GetDiskFreeSpaceExW(outputDirectory.c_str(), &availableBytes, nullptr, nullptr) == FALSE)
    {
        return LastError(StorageStage::Preallocation);
    }
    if (fileBytes > availableBytes.QuadPart)
    {
        return StorageStatus::Failure(StorageErrorCode::ResourceLimit, StorageStage::Preallocation, ERROR_DISK_FULL);
    }

    FILE_ALLOCATION_INFO allocationInfo{};
    allocationInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(fileBytes);
    if (SetFileInformationByHandle(handle, FileAllocationInfo, &allocationInfo, sizeof(allocationInfo)) == FALSE)
    {
        return LastError(StorageStage::Preallocation);
    }
    LARGE_INTEGER finalSize{};
    finalSize.QuadPart = static_cast<LONGLONG>(fileBytes);
    if (SetFilePointerEx(handle, finalSize, nullptr, FILE_BEGIN) == FALSE ||
        SetEndOfFile(handle) == FALSE || FlushFileBuffers(handle) == FALSE)
    {
        return LastError(StorageStage::Preallocation);
    }
    return StorageStatus::Success();
}

} // namespace

struct OutputFile::Implementation
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring finalPath;
    std::wstring partPath;
    std::uint64_t fileBytes = 0;
    std::uint64_t verifiedBytes = 0;
    std::vector<VerifiedRange> verifiedRanges;
    std::optional<VerifiedRange> pendingRange;
    bool resumed = false;
    bool ownsPart = false;
    bool published = false;
};

StorageStatus StorageStatus::Failure(const StorageErrorCode code, const StorageStage stage,
    const std::int32_t nativeError) noexcept
{
    return {code == StorageErrorCode::None ? StorageErrorCode::InternalError : code, stage, nativeError};
}

const char* GetStorageErrorName(const StorageErrorCode code) noexcept
{
    switch (code)
    {
    case StorageErrorCode::None: return "None";
    case StorageErrorCode::InvalidArgument: return "InvalidArgument";
    case StorageErrorCode::InvalidDirectory: return "InvalidDirectory";
    case StorageErrorCode::TargetExists: return "TargetExists";
    case StorageErrorCode::ResourceLimit: return "ResourceLimit";
    case StorageErrorCode::NativeFailure: return "NativeFailure";
    case StorageErrorCode::DigestMismatch: return "DigestMismatch";
    case StorageErrorCode::AlreadyPublished: return "AlreadyPublished";
    case StorageErrorCode::OutOfMemory: return "OutOfMemory";
    case StorageErrorCode::InternalError: return "InternalError";
    case StorageErrorCode::ResumeMismatch: return "ResumeMismatch";
    case StorageErrorCode::OverlappingRange: return "OverlappingRange";
    case StorageErrorCode::IncompleteFile: return "IncompleteFile";
    }
    return "Unknown";
}

const char* GetStorageStageName(const StorageStage stage) noexcept
{
    switch (stage)
    {
    case StorageStage::None: return "None";
    case StorageStage::Configuration: return "Configuration";
    case StorageStage::Directory: return "Directory";
    case StorageStage::Reservation: return "Reservation";
    case StorageStage::Preallocation: return "Preallocation";
    case StorageStage::Write: return "Write";
    case StorageStage::Flush: return "Flush";
    case StorageStage::Digest: return "Digest";
    case StorageStage::Publish: return "Publish";
    case StorageStage::Cleanup: return "Cleanup";
    case StorageStage::Resume: return "Resume";
    }
    return "Unknown";
}

StorageStatus OutputFile::CreateInternal(const OutputFileConfig& config,
    const bool allowResume, std::unique_ptr<OutputFile>& output) noexcept
{
    output.reset();
    if (config.outputDirectory.empty() || config.maximumFileBytes == 0 ||
        config.fileBytes > config.maximumFileBytes ||
        config.fileBytes > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
    {
        return StorageStatus::Failure(config.fileBytes > config.maximumFileBytes ? StorageErrorCode::ResourceLimit :
            StorageErrorCode::InvalidArgument, StorageStage::Configuration);
    }
    DWORD directoryError = ERROR_SUCCESS;
    if (!PathExists(config.outputDirectory, directoryError))
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidDirectory, StorageStage::Directory,
            static_cast<std::int32_t>(directoryError));
    }
    const DWORD attributes = GetFileAttributesW(config.outputDirectory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidDirectory, StorageStage::Directory,
            attributes == INVALID_FILE_ATTRIBUTES ? static_cast<std::int32_t>(GetLastError()) : ERROR_DIRECTORY);
    }

    try
    {
        auto implementation = std::make_unique<OutputFile::Implementation>();
        const std::filesystem::path directory(config.outputDirectory);
        StorageStatus status = SelectFinalPath(config, directory, implementation->finalPath);
        if (!status)
        {
            return status;
        }
        implementation->partPath = (directory / MakePartName(config.sessionTag)).wstring();
        implementation->fileBytes = config.fileBytes;

        DWORD partError = ERROR_SUCCESS;
        const bool partExists = PathExists(implementation->partPath, partError);
        if (!partExists && partError != ERROR_FILE_NOT_FOUND && partError != ERROR_PATH_NOT_FOUND)
        {
            return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Reservation,
                static_cast<std::int32_t>(partError));
        }
        if (partExists && !allowResume)
        {
            return StorageStatus::Failure(StorageErrorCode::TargetExists, StorageStage::Reservation, ERROR_FILE_EXISTS);
        }

        implementation->handle = CreateFileW(implementation->partPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, partExists ? OPEN_EXISTING : CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (implementation->handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return StorageStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                StorageErrorCode::TargetExists : StorageErrorCode::NativeFailure,
                partExists ? StorageStage::Resume : StorageStage::Reservation,
                static_cast<std::int32_t>(error));
        }
        implementation->ownsPart = true;
        implementation->resumed = partExists;

        if (partExists)
        {
            std::uint64_t existingBytes = 0;
            status = GetFileLength(implementation->handle, existingBytes, StorageStage::Resume);
            if (!status || existingBytes != config.fileBytes)
            {
                CloseHandle(implementation->handle);
                implementation->handle = INVALID_HANDLE_VALUE;
                return status ? StorageStatus::Failure(StorageErrorCode::ResumeMismatch, StorageStage::Resume) : status;
            }
        }
        else
        {
            status = ReserveNewFile(implementation->handle, config.outputDirectory, config.fileBytes);
            if (!status)
            {
                CloseHandle(implementation->handle);
                implementation->handle = INVALID_HANDLE_VALUE;
                DeleteFileW(implementation->partPath.c_str());
                implementation->ownsPart = false;
                return status;
            }
        }

        output = std::unique_ptr<OutputFile>(new OutputFile(std::move(implementation)));
        return StorageStatus::Success();
    }
    catch (const std::bad_alloc&)
    {
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Reservation);
    }
    catch (...)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Directory);
    }
}

StorageStatus OutputFile::Create(const OutputFileConfig& config,
    std::unique_ptr<OutputFile>& output) noexcept
{
    return CreateInternal(config, false, output);
}

StorageStatus OutputFile::CreateOrResume(const OutputFileConfig& config,
    std::unique_ptr<OutputFile>& output) noexcept
{
    return CreateInternal(config, true, output);
}

OutputFile::OutputFile(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

OutputFile::~OutputFile()
{
    if (implementation_ && implementation_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(implementation_->handle);
        implementation_->handle = INVALID_HANDLE_VALUE;
    }
}

StorageStatus OutputFile::WriteVerifiedSegment(const std::uint64_t rawOffset,
    const std::span<const std::byte> rawBytes) noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Write);
    }
    if (implementation_->pendingRange.has_value() || rawBytes.empty())
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Write);
    }
    const auto endResult = pbprotocol::CheckedAddUint64(rawOffset, rawBytes.size());
    if (!endResult || endResult.Value() > implementation_->fileBytes ||
        rawOffset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Write);
    }
    const VerifiedRange range{rawOffset, endResult.Value()};
    const auto position = std::lower_bound(implementation_->verifiedRanges.begin(),
        implementation_->verifiedRanges.end(), range.begin,
        [](const VerifiedRange& existing, const std::uint64_t begin)
        {
            return existing.begin < begin;
        });
    if ((position != implementation_->verifiedRanges.end() && range.end > position->begin) ||
        (position != implementation_->verifiedRanges.begin() && std::prev(position)->end > range.begin))
    {
        return StorageStatus::Failure(StorageErrorCode::OverlappingRange, StorageStage::Write);
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(rawOffset);
    if (SetFilePointerEx(implementation_->handle, offset, nullptr, FILE_BEGIN) == FALSE)
    {
        return LastError(StorageStage::Write);
    }
    std::size_t consumed = 0;
    while (consumed < rawBytes.size())
    {
        const DWORD chunkBytes = static_cast<DWORD>((std::min)(rawBytes.size() - consumed,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD writtenBytes = 0;
        if (WriteFile(implementation_->handle, rawBytes.data() + consumed, chunkBytes, &writtenBytes, nullptr) == FALSE ||
            writtenBytes != chunkBytes)
        {
            return LastError(StorageStage::Write);
        }
        consumed += writtenBytes;
    }
    implementation_->pendingRange = range;
    return StorageStatus::Success();
}

StorageStatus OutputFile::FlushVerifiedSegment() noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Flush);
    }
    if (!implementation_->pendingRange.has_value())
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Flush);
    }
    if (FlushFileBuffers(implementation_->handle) == FALSE)
    {
        return LastError(StorageStage::Flush);
    }

    const VerifiedRange pending = *implementation_->pendingRange;
    const auto position = std::lower_bound(implementation_->verifiedRanges.begin(),
        implementation_->verifiedRanges.end(), pending.begin,
        [](const VerifiedRange& existing, const std::uint64_t begin)
        {
            return existing.begin < begin;
        });
    auto inserted = implementation_->verifiedRanges.insert(position, pending);
    if (inserted != implementation_->verifiedRanges.begin())
    {
        auto previous = std::prev(inserted);
        if (previous->end == inserted->begin)
        {
            previous->end = inserted->end;
            inserted = implementation_->verifiedRanges.erase(inserted);
            inserted = previous;
        }
    }
    const auto next = std::next(inserted);
    if (next != implementation_->verifiedRanges.end() && inserted->end == next->begin)
    {
        inserted->end = next->end;
        implementation_->verifiedRanges.erase(next);
    }
    implementation_->verifiedBytes += pending.end - pending.begin;
    implementation_->pendingRange.reset();
    return StorageStatus::Success();
}

StorageStatus OutputFile::ReadRange(const std::uint64_t rawOffset, const std::span<std::byte> output) noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published ||
        implementation_->pendingRange.has_value())
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Resume);
    }
    const auto endResult = pbprotocol::CheckedAddUint64(rawOffset, output.size());
    if (!endResult || endResult.Value() > implementation_->fileBytes ||
        rawOffset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Resume);
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(rawOffset);
    if (SetFilePointerEx(implementation_->handle, offset, nullptr, FILE_BEGIN) == FALSE)
    {
        return LastError(StorageStage::Resume);
    }
    std::size_t consumed = 0;
    while (consumed < output.size())
    {
        const DWORD chunkBytes = static_cast<DWORD>((std::min)(output.size() - consumed,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD readBytes = 0;
        if (ReadFile(implementation_->handle, output.data() + consumed, chunkBytes, &readBytes, nullptr) == FALSE ||
            readBytes != chunkBytes)
        {
            return LastError(StorageStage::Resume);
        }
        consumed += readBytes;
    }
    return StorageStatus::Success();
}

StorageStatus OutputFile::AdoptVerifiedSegment(const std::uint64_t rawOffset, const std::uint64_t rawSize) noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published ||
        implementation_->pendingRange.has_value() || rawSize == 0)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Resume);
    }
    const auto endResult = pbprotocol::CheckedAddUint64(rawOffset, rawSize);
    if (!endResult || endResult.Value() > implementation_->fileBytes)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Resume);
    }
    const VerifiedRange range{rawOffset, endResult.Value()};
    const auto position = std::lower_bound(implementation_->verifiedRanges.begin(),
        implementation_->verifiedRanges.end(), range.begin,
        [](const VerifiedRange& existing, const std::uint64_t begin)
        {
            return existing.begin < begin;
        });
    if ((position != implementation_->verifiedRanges.end() && range.end > position->begin) ||
        (position != implementation_->verifiedRanges.begin() && std::prev(position)->end > range.begin))
    {
        return StorageStatus::Failure(StorageErrorCode::OverlappingRange, StorageStage::Resume);
    }
    try
    {
        auto inserted = implementation_->verifiedRanges.insert(position, range);
        if (inserted != implementation_->verifiedRanges.begin())
        {
            auto previous = std::prev(inserted);
            if (previous->end == inserted->begin)
            {
                previous->end = inserted->end;
                inserted = implementation_->verifiedRanges.erase(inserted);
                inserted = previous;
            }
        }
        const auto next = std::next(inserted);
        if (next != implementation_->verifiedRanges.end() && inserted->end == next->begin)
        {
            inserted->end = next->end;
            implementation_->verifiedRanges.erase(next);
        }
    }
    catch (const std::bad_alloc&)
    {
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Resume);
    }
    implementation_->verifiedBytes += rawSize;
    return StorageStatus::Success();
}

StorageStatus OutputFile::Checkpoint() noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Flush);
    }
    if (implementation_->pendingRange.has_value())
    {
        return FlushVerifiedSegment();
    }
    if (FlushFileBuffers(implementation_->handle) == FALSE)
    {
        return LastError(StorageStage::Flush);
    }
    return StorageStatus::Success();
}

StorageStatus OutputFile::Write(const std::uint64_t rawOffset,
    const std::span<const std::byte> rawBytes) noexcept
{
    if (!implementation_ || implementation_->pendingRange.has_value() || rawOffset != implementation_->verifiedBytes)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Write);
    }
    if (rawOffset != 0 && (implementation_->verifiedRanges.size() != 1 ||
        implementation_->verifiedRanges.front().begin != 0 || implementation_->verifiedRanges.front().end != rawOffset))
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Write);
    }
    const StorageStatus writeStatus = WriteVerifiedSegment(rawOffset, rawBytes);
    if (!writeStatus)
    {
        return writeStatus;
    }
    return FlushVerifiedSegment();
}

StorageStatus OutputFile::Publish(const pbprotocol::WholeFileDigest& expectedDigest) noexcept
{
    if (!implementation_ || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Publish);
    }
    if (implementation_->handle == INVALID_HANDLE_VALUE || implementation_->pendingRange.has_value() ||
        implementation_->verifiedBytes != implementation_->fileBytes)
    {
        return StorageStatus::Failure(StorageErrorCode::IncompleteFile, StorageStage::Publish);
    }
    if (FlushFileBuffers(implementation_->handle) == FALSE)
    {
        return LastError(StorageStage::Flush);
    }
    if (CloseHandle(implementation_->handle) == FALSE)
    {
        implementation_->handle = INVALID_HANDLE_VALUE;
        return LastError(StorageStage::Flush);
    }
    implementation_->handle = INVALID_HANDLE_VALUE;

    std::array<std::byte, pbprotocol::kDigestBytes> partDigest{};
    std::uint64_t partBytes = 0;
    StorageStatus status = HashFile(implementation_->partPath, partDigest, partBytes);
    if (!status)
    {
        return status;
    }
    if (partBytes != implementation_->fileBytes || partDigest != expectedDigest.bytes)
    {
        return StorageStatus::Failure(StorageErrorCode::DigestMismatch, StorageStage::Digest);
    }
    status = RequireAbsent(implementation_->finalPath);
    if (!status)
    {
        return status;
    }
    if (MoveFileExW(implementation_->partPath.c_str(), implementation_->finalPath.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        return LastError(StorageStage::Publish);
    }

    std::array<std::byte, pbprotocol::kDigestBytes> finalDigest{};
    std::uint64_t finalBytes = 0;
    status = HashFile(implementation_->finalPath, finalDigest, finalBytes);
    if (!status || finalBytes != implementation_->fileBytes || finalDigest != expectedDigest.bytes)
    {
        const StorageStatus verificationStatus = !status ? status :
            StorageStatus::Failure(StorageErrorCode::DigestMismatch, StorageStage::Publish);
        if (MoveFileExW(implementation_->finalPath.c_str(), implementation_->partPath.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Cleanup,
                static_cast<std::int32_t>(GetLastError()));
        }
        return verificationStatus;
    }
    implementation_->published = true;
    implementation_->ownsPart = false;
    return StorageStatus::Success();
}

StorageStatus OutputFile::Discard() noexcept
{
    if (!implementation_ || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Cleanup);
    }
    if (implementation_->handle != INVALID_HANDLE_VALUE)
    {
        if (CloseHandle(implementation_->handle) == FALSE)
        {
            implementation_->handle = INVALID_HANDLE_VALUE;
            return LastError(StorageStage::Cleanup);
        }
        implementation_->handle = INVALID_HANDLE_VALUE;
    }
    if (implementation_->ownsPart && DeleteFileW(implementation_->partPath.c_str()) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Cleanup,
                static_cast<std::int32_t>(error));
        }
    }
    implementation_->ownsPart = false;
    return StorageStatus::Success();
}

OutputFileSnapshot OutputFile::GetSnapshot() const
{
    if (!implementation_)
    {
        return {};
    }
    return {implementation_->finalPath, implementation_->partPath, implementation_->fileBytes,
        implementation_->verifiedBytes, implementation_->verifiedBytes, implementation_->resumed,
        implementation_->pendingRange.has_value(), implementation_->published};
}

} // namespace pbstorage
