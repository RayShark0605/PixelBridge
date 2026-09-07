#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>

namespace pbapp
{

[[nodiscard]] inline bool IsValidStandardHandle(const HANDLE handle) noexcept
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(handle);
    return type != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
}

[[nodiscard]] inline bool BindMissingStandardStream(FILE* const stream, const DWORD standardId, const bool input) noexcept
{
    const int descriptor = _fileno(stream);
    if (descriptor >= 0 && IsValidStandardHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor))))
    {
        return true;
    }
    const HANDLE handle = GetStdHandle(standardId);
    if (!IsValidStandardHandle(handle))
    {
        // A detached CLI with no parent console and no redirection has no
        // destination. Never create a new console just to display a message.
        return true;
    }
    HANDLE duplicate = nullptr;
    const HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process, handle, process, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        return false;
    }
    const int duplicateDescriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(duplicate), _O_TEXT | (input ? _O_RDONLY : _O_WRONLY));
    if (duplicateDescriptor < 0)
    {
        CloseHandle(duplicate);
        return false;
    }
    // A GUI-subsystem CRT can represent a missing stream with descriptor -2.
    // freopen gives that FILE a real descriptor before dup2 binds the owned
    // copy. The inherited redirection itself must not be closed or replaced.
    FILE* rebound = nullptr;
    const bool reopened = _wfreopen_s(&rebound, L"NUL", input ? L"r" : L"w", stream) == 0;
    const bool bound = reopened && _dup2(duplicateDescriptor, _fileno(stream)) == 0;
    static_cast<void>(_close(duplicateDescriptor));
    if (!bound)
    {
        return false;
    }
    clearerr(stream);
    return SetStdHandle(standardId, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)))) != FALSE;
}

[[nodiscard]] inline bool PrepareCommandLineStreams() noexcept
{
    constexpr std::array<DWORD, 3> standardIds{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    std::array<HANDLE, 3> redirected{};
    for (std::size_t index = 0; index < standardIds.size(); index++)
    {
        const HANDLE handle = GetStdHandle(standardIds[index]);
        DWORD mode = 0;
        if (IsValidStandardHandle(handle) && !GetConsoleMode(handle, &mode))
        {
            redirected[index] = handle;
        }
    }
    // Attach only to the invoking terminal, never AllocConsole or activate a
    // window. Attachment may replace Win32 standard handles; preserve each
    // independently redirected pipe/file/NUL endpoint before repairing CRT.
    static_cast<void>(AttachConsole(ATTACH_PARENT_PROCESS));
    bool ready = true;
    for (std::size_t index = 0; index < standardIds.size(); index++)
    {
        if (redirected[index] != nullptr && !SetStdHandle(standardIds[index], redirected[index]))
        {
            ready = false;
        }
    }
    ready = BindMissingStandardStream(stdin, STD_INPUT_HANDLE, true) && ready;
    ready = BindMissingStandardStream(stdout, STD_OUTPUT_HANDLE, false) && ready;
    ready = BindMissingStandardStream(stderr, STD_ERROR_HANDLE, false) && ready;
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    return ready;
}

} // namespace pbapp
