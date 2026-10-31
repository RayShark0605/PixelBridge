#include "feedback_file_store.h"
#include "feedback_win32.h"

#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/session_random.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace pbapp
{
namespace
{

FeedbackStatus Failure(const FeedbackError error, const char* const message, const DWORD native = 0) noexcept
{
    return {error, message, native};
}

FeedbackStatus ValidateRegularFile(const HANDLE file) noexcept
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &information))
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback must be a regular disk file", GetLastError());
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        information.nNumberOfLinks != 1)
    {
        return Failure(FeedbackError::InvalidFormat, "Feedback aliases and directories are not accepted");
    }
    return {};
}

FeedbackStatus ValidateParent(const HANDLE parent) noexcept
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(parent, &information))
    {
        return Failure(FeedbackError::IoFailure, "Feedback parent query failed", GetLastError());
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return Failure(FeedbackError::InvalidPath, "Feedback parent must be an ordinary selected directory");
    }
    return {};
}

FeedbackStatus RenameOwnedSnapshot(const HANDLE file, const std::filesystem::path& path) noexcept
{
    try
    {
        const auto& name = path.native();
        const std::size_t nameBytes = name.size() * sizeof(wchar_t);
        const std::size_t totalBytes = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
        std::vector<std::byte> storage(totalBytes);
        auto* const information = new (storage.data()) FILE_RENAME_INFO{};
        information->Flags = 0;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, name.data(), nameBytes);
        if (!SetFileInformationByHandle(file, FileRenameInfoEx, information, static_cast<DWORD>(totalBytes)))
        {
            const DWORD error = GetLastError();
            return Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? FeedbackError::Conflict : FeedbackError::IoFailure,
                "Feedback no-replace publication failed", error);
        }
        return {};
    }
    catch (...)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback rename allocation failed");
    }
}

void DiscardOwnedTemporary(const HANDLE file) noexcept
{
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    static_cast<void>(SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)));
}

FeedbackStatus ReadSnapshotHandle(const HANDLE file, std::vector<std::byte>& output) noexcept
{
    const auto regular = ValidateRegularFile(file);
    if (!regular)
    {
        return regular;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size))
    {
        return Failure(FeedbackError::IoFailure, "Feedback read size failed", GetLastError());
    }
    if (size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > maximumFeedbackSnapshotBytes)
    {
        return Failure(FeedbackError::LimitExceeded, "Feedback read length is outside limits");
    }
    try
    {
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD readBytes = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &readBytes, nullptr) || readBytes != bytes.size())
        {
            return Failure(FeedbackError::IoFailure, "Feedback read was incomplete", GetLastError());
        }
        LARGE_INTEGER finalSize{};
        if (!GetFileSizeEx(file, &finalSize) || finalSize.QuadPart != size.QuadPart)
        {
            return Failure(FeedbackError::IoFailure, "Feedback file length changed during read", GetLastError());
        }
        FeedbackSnapshot parsed;
        const auto parsedStatus = ParseFeedbackSnapshot(bytes, parsed);
        if (!parsedStatus)
        {
            return parsedStatus;
        }
        output.swap(bytes);
        return {};
    }
    catch (...)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback read allocation failed");
    }
}

}

struct FeedbackFilePublisher::Implementation
{
    std::filesystem::path path;
    std::filesystem::path temporaryPath;
    std::filesystem::path previousPath;
    detail::FeedbackHandle parent;
    detail::FeedbackHandle lock;
    std::array<std::byte, 16> receiverId{};
    bool primaryExists = false;
    bool conflictLatched = false;
};

FeedbackFilePublisher::FeedbackFilePublisher() = default;
FeedbackFilePublisher::~FeedbackFilePublisher() = default;
FeedbackFilePublisher::FeedbackFilePublisher(FeedbackFilePublisher&&) noexcept = default;
FeedbackFilePublisher& FeedbackFilePublisher::operator=(FeedbackFilePublisher&&) noexcept = default;

FeedbackStatus ResolveFeedbackFilePath(const FeedbackConfig& config, std::filesystem::path& output) noexcept
{
    try
    {
        if (config.path.empty() || !config.path.is_absolute() || config.path.native().size() > 32700 ||
            config.path.native().find(L'\0') != std::wstring::npos ||
            (config.locationKind != FeedbackLocationKind::Directory && config.locationKind != FeedbackLocationKind::File))
        {
            return Failure(FeedbackError::InvalidPath, "Feedback path must be a bounded absolute path");
        }
        auto path = config.locationKind == FeedbackLocationKind::Directory ? config.path / L"PixelBridgeFeedback.pbfb" : config.path;
        const auto& native = path.native();
        if (native.find(L':', path.root_name().native().size()) != std::wstring::npos)
        {
            return Failure(FeedbackError::InvalidPath, "Feedback alternate streams are not accepted");
        }
        const std::wstring name = path.filename().native();
        if (name.empty() || name.size() > 240)
        {
            return Failure(FeedbackError::InvalidPath, "Feedback filename is invalid");
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
        {
            return Failure(FeedbackError::InvalidPath, "Feedback filename is not valid Unicode");
        }
        std::string utf8(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), utf8.data(), bytes, nullptr, nullptr) != bytes ||
            !pbprotocol::ValidateFileNameUtf8(utf8))
        {
            return Failure(FeedbackError::InvalidPath, "Feedback filename is unsafe");
        }
        output.swap(path);
        return {};
    }
    catch (...)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback path allocation failed");
    }
}

FeedbackStatus ReadFeedbackFile(const std::filesystem::path& path, std::vector<std::byte>& output) noexcept
{
    std::filesystem::path resolved;
    const auto pathStatus = ResolveFeedbackFilePath({path, FeedbackLocationKind::File}, resolved);
    if (!pathStatus)
    {
        return pathStatus;
    }
    const detail::FeedbackHandle file(CreateFileW(resolved.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file)
    {
        return Failure(FeedbackError::Unavailable, "Feedback read open failed", GetLastError());
    }
    return ReadSnapshotHandle(file.Get(), output);
}

FeedbackStatus FeedbackFilePublisher::Open(const std::filesystem::path& path) noexcept
{
    if (implementation_)
    {
        return Failure(FeedbackError::Conflict, "Feedback publisher is already open");
    }
    try
    {
        auto state = std::make_unique<Implementation>();
        const auto pathStatus = ResolveFeedbackFilePath({path, FeedbackLocationKind::File}, state->path);
        if (!pathStatus)
        {
            return pathStatus;
        }
        state->temporaryPath = state->path.wstring() + L".tmp";
        state->previousPath = state->path.wstring() + L".previous";
        state->parent.Reset(CreateFileW(state->path.parent_path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!state->parent)
        {
            return Failure(FeedbackError::Unavailable, "Feedback parent is unavailable", GetLastError());
        }
        const auto parentStatus = ValidateParent(state->parent.Get());
        if (!parentStatus)
        {
            return parentStatus;
        }
        const auto lockPath = state->path.wstring() + L".lock";
        state->lock.Reset(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!state->lock)
        {
            return Failure(FeedbackError::Conflict, "Feedback writer lock is busy or unowned", GetLastError());
        }
        const auto lockStatus = ValidateRegularFile(state->lock.Get());
        if (!lockStatus)
        {
            return lockStatus;
        }
        const DWORD previousAttributes = GetFileAttributesW(state->previousPath.c_str());
        if (previousAttributes != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            return Failure(FeedbackError::Conflict, "Feedback previous name is occupied or unavailable");
        }
        std::vector<std::byte> previous;
        const auto readStatus = ReadFeedbackFile(state->path, previous);
        if (readStatus)
        {
            FeedbackSnapshot parsed;
            const auto parsedStatus = ParseFeedbackSnapshot(previous, parsed);
            if (!parsedStatus)
            {
                return parsedStatus;
            }
            state->receiverId = parsed.receiverId;
            state->primaryExists = true;
        }
        else if (readStatus.nativeError == ERROR_FILE_NOT_FOUND)
        {
            const auto random = pbprotocol::GenerateRandomSessionId();
            if (!random)
            {
                return Failure(FeedbackError::Unavailable, "Feedback writer random identity failed");
            }
            state->receiverId = random.Value().bytes;
        }
        else
        {
            return readStatus;
        }
        implementation_ = std::move(state);
        return {};
    }
    catch (...)
    {
        return Failure(FeedbackError::AllocationFailure, "Feedback publisher allocation failed");
    }
}

FeedbackStatus FeedbackFilePublisher::Publish(const FeedbackSnapshot& snapshot) noexcept
{
    return PublishInternal(snapshot, nullptr, nullptr);
}

FeedbackStatus FeedbackFilePublisher::PublishInternal(const FeedbackSnapshot& snapshot, void (*const beforeCommit)(void*), void* const context) noexcept
{
    if (!implementation_)
    {
        return Failure(FeedbackError::Unavailable, "Feedback publisher is not open");
    }
    auto& state = *implementation_;
    if (state.conflictLatched)
    {
        return Failure(FeedbackError::Conflict, "Feedback publication requires explicit conflict recovery");
    }
    if (snapshot.receiverId != state.receiverId)
    {
        return Failure(FeedbackError::Conflict, "Feedback writer identity does not match owner");
    }
    std::vector<std::byte> encoded;
    const auto serialized = SerializeFeedbackSnapshot(snapshot, encoded);
    if (!serialized)
    {
        return serialized;
    }
    detail::FeedbackHandle previous;
    if (state.primaryExists)
    {
        previous.Reset(CreateFileW(state.path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!previous)
        {
            return Failure(FeedbackError::Unavailable, "Feedback primary ownership lease failed", GetLastError());
        }
        std::vector<std::byte> previousBytes;
        const auto readStatus = ReadSnapshotHandle(previous.Get(), previousBytes);
        if (!readStatus)
        {
            return readStatus;
        }
        FeedbackSnapshot parsed;
        const auto parsedStatus = ParseFeedbackSnapshot(previousBytes, parsed);
        if (!parsedStatus || parsed.receiverId != state.receiverId)
        {
            return Failure(FeedbackError::Conflict, "Feedback primary was replaced by another owner");
        }
    }
    detail::FeedbackHandle temporary(CreateFileW(state.temporaryPath.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!temporary)
    {
        return Failure(FeedbackError::Conflict, "Feedback temporary name is occupied", GetLastError());
    }
    const auto regular = ValidateRegularFile(temporary.Get());
    DWORD writtenBytes = 0;
    const bool written = regular && WriteFile(temporary.Get(), encoded.data(), static_cast<DWORD>(encoded.size()), &writtenBytes, nullptr) &&
        writtenBytes == encoded.size() && FlushFileBuffers(temporary.Get());
    const DWORD writeError = written ? ERROR_SUCCESS : GetLastError();
    if (!written)
    {
        DiscardOwnedTemporary(temporary.Get());
        return Failure(FeedbackError::IoFailure, "Feedback snapshot write or flush failed", writeError);
    }
    if (previous)
    {
        // Win32 pathname replacement has no expected-object CAS. Retire only
        // the exact validated, write-denying object; never replace a leaf.
        const auto retired = RenameOwnedSnapshot(previous.Get(), state.previousPath);
        if (!retired)
        {
            DiscardOwnedTemporary(temporary.Get());
            return retired;
        }
    }
    if (beforeCommit)
    {
        beforeCommit(context);
    }
    const auto published = RenameOwnedSnapshot(temporary.Get(), state.path);
    if (!published)
    {
        DiscardOwnedTemporary(temporary.Get());
        if (previous && !RenameOwnedSnapshot(previous.Get(), state.path))
        {
            // An unrelated new target is left untouched; retain one complete
            // prior snapshot rather than deleting it or inventing ownership.
            state.conflictLatched = true;
            return Failure(FeedbackError::Conflict, "Feedback target changed; previous snapshot retained", published.nativeError);
        }
        return published;
    }
    state.primaryExists = true;
    if (previous)
    {
        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
        if (!SetFileInformationByHandle(previous.Get(), FileDispositionInfoEx, &disposition, sizeof(disposition)))
        {
            state.conflictLatched = true;
            return Failure(FeedbackError::IoFailure, "Feedback previous-object cleanup failed", GetLastError());
        }
    }
    return {};
}

std::array<std::byte, 16> FeedbackFilePublisher::GetReceiverId() const noexcept
{
    return implementation_ ? implementation_->receiverId : std::array<std::byte, 16>{};
}

void FeedbackFilePublisher::Close() noexcept
{
    implementation_.reset();
}

}
