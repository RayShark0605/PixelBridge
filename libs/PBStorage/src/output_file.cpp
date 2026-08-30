#include "pbstorage/output_file.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

namespace pbstorage
{
namespace
{

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
        return StorageStatus::Failure(StorageErrorCode::TargetExists, StorageStage::Reservation,
            ERROR_FILE_EXISTS);
    }
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
        return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Reservation,
            static_cast<std::int32_t>(error));
    }
    return StorageStatus::Success();
}

[[nodiscard]] std::wstring MakeFileName(const pbprotocol::SessionTag sessionTag)
{
    std::wostringstream stream;
    stream << L"PixelBridge-" << std::hex << std::setw(16) << std::setfill(L'0') << sessionTag.value << L".bin";
    return stream.str();
}

[[nodiscard]] StorageStatus HashFile(const std::wstring& path,
    std::array<std::byte, pbprotocol::kDigestBytes>& output) noexcept
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return LastError(StorageStage::Digest);
    }
    pbprotocol::Blake3Hasher hasher;
    std::vector<std::byte> buffer;
    try
    {
        buffer.resize(64U * 1024U);
    }
    catch (const std::bad_alloc&)
    {
        CloseHandle(handle);
        return StorageStatus::Failure(StorageErrorCode::OutOfMemory, StorageStage::Digest);
    }
    StorageStatus status = StorageStatus::Success();
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

} // namespace

struct OutputFile::Implementation
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring finalPath;
    std::wstring partPath;
    std::uint64_t fileBytes = 0;
    std::uint64_t writtenBytes = 0;
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
    }
    return "Unknown";
}

StorageStatus OutputFile::Create(const OutputFileConfig& config,
    std::unique_ptr<OutputFile>& output) noexcept
{
    if (config.outputDirectory.empty() || config.fileBytes == 0 ||
        config.maximumFileBytes == 0 || config.fileBytes > config.maximumFileBytes ||
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
        auto implementation = std::make_unique<Implementation>();
        const std::filesystem::path directory(config.outputDirectory);
        implementation->finalPath = (directory / MakeFileName(config.sessionTag)).wstring();
        implementation->partPath = implementation->finalPath + L".part";
        implementation->fileBytes = config.fileBytes;
        auto candidate = std::unique_ptr<OutputFile>(new OutputFile(std::move(implementation)));
        Implementation& state = *candidate->implementation_;
        auto status = RequireAbsent(state.finalPath);
        if (!status)
        {
            return status;
        }
        status = RequireAbsent(state.partPath);
        if (!status)
        {
            return status;
        }
        state.handle = CreateFileW(state.partPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (state.handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return StorageStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                StorageErrorCode::TargetExists : StorageErrorCode::NativeFailure, StorageStage::Reservation,
                static_cast<std::int32_t>(error));
        }
        state.ownsPart = true;
        LARGE_INTEGER finalSize{};
        finalSize.QuadPart = static_cast<LONGLONG>(config.fileBytes);
        if (SetFilePointerEx(state.handle, finalSize, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(state.handle) == FALSE || FlushFileBuffers(state.handle) == FALSE)
        {
            status = LastError(StorageStage::Preallocation);
            DWORD cleanupError = ERROR_SUCCESS;
            if (CloseHandle(state.handle) == FALSE)
            {
                cleanupError = GetLastError();
            }
            state.handle = INVALID_HANDLE_VALUE;
            if (DeleteFileW(state.partPath.c_str()) == FALSE)
            {
                const DWORD deleteError = GetLastError();
                if (deleteError != ERROR_FILE_NOT_FOUND && deleteError != ERROR_PATH_NOT_FOUND)
                {
                    cleanupError = deleteError;
                }
            }
            else
            {
                state.ownsPart = false;
            }
            if (cleanupError != ERROR_SUCCESS)
            {
                return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Cleanup,
                    static_cast<std::int32_t>(cleanupError));
            }
            return status;
        }
        output = std::move(candidate);
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

OutputFile::OutputFile(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

OutputFile::~OutputFile()
{
    if (!implementation_)
    {
        return;
    }
    if (implementation_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(implementation_->handle);
        implementation_->handle = INVALID_HANDLE_VALUE;
    }
    if (implementation_->ownsPart && !implementation_->published && !implementation_->partPath.empty())
    {
        DeleteFileW(implementation_->partPath.c_str());
    }
}

StorageStatus OutputFile::Write(const std::uint64_t rawOffset,
    const std::span<const std::byte> rawBytes) noexcept
{
    if (!implementation_ || implementation_->handle == INVALID_HANDLE_VALUE || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Write);
    }
    const auto end = pbprotocol::CheckedAddUint64(rawOffset, rawBytes.size());
    if (rawBytes.empty() || rawOffset != implementation_->writtenBytes || !end ||
        end.Value() > implementation_->fileBytes ||
        rawBytes.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Write);
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
        const DWORD chunk = static_cast<DWORD>((std::min)(rawBytes.size() - consumed,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (WriteFile(implementation_->handle, rawBytes.data() + consumed, chunk, &written, nullptr) == FALSE ||
            written != chunk)
        {
            return LastError(StorageStage::Write);
        }
        consumed += written;
    }
    if (FlushFileBuffers(implementation_->handle) == FALSE)
    {
        return LastError(StorageStage::Flush);
    }
    implementation_->writtenBytes = end.Value();
    return StorageStatus::Success();
}

StorageStatus OutputFile::Publish(const pbprotocol::WholeFileDigest& expectedDigest) noexcept
{
    if (!implementation_ || implementation_->published)
    {
        return StorageStatus::Failure(StorageErrorCode::AlreadyPublished, StorageStage::Publish);
    }
    if (implementation_->handle == INVALID_HANDLE_VALUE || implementation_->writtenBytes != implementation_->fileBytes)
    {
        return StorageStatus::Failure(StorageErrorCode::InvalidArgument, StorageStage::Publish);
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
    auto status = HashFile(implementation_->partPath, partDigest);
    if (!status)
    {
        return status;
    }
    if (partDigest != expectedDigest.bytes)
    {
        return StorageStatus::Failure(StorageErrorCode::DigestMismatch, StorageStage::Digest);
    }
    status = RequireAbsent(implementation_->finalPath);
    if (!status)
    {
        return status;
    }
    if (MoveFileExW(implementation_->partPath.c_str(), implementation_->finalPath.c_str(),
        MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        return LastError(StorageStage::Publish);
    }
    std::array<std::byte, pbprotocol::kDigestBytes> finalDigest{};
    status = HashFile(implementation_->finalPath, finalDigest);
    if (!status)
    {
        const StorageStatus hashStatus = status;
        if (DeleteFileW(implementation_->finalPath.c_str()) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            {
                return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Cleanup,
                    static_cast<std::int32_t>(error));
            }
        }
        return hashStatus;
    }
    if (finalDigest != expectedDigest.bytes)
    {
        if (DeleteFileW(implementation_->finalPath.c_str()) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            {
                return StorageStatus::Failure(StorageErrorCode::NativeFailure, StorageStage::Cleanup,
                    static_cast<std::int32_t>(error));
            }
        }
        return StorageStatus::Failure(StorageErrorCode::DigestMismatch, StorageStage::Publish);
    }
    implementation_->published = true;
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
        implementation_->writtenBytes, implementation_->published};
}

} // namespace pbstorage
