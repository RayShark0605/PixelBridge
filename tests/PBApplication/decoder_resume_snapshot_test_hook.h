#pragma once

#ifndef PB_RESUME_BATCH_TESTS
#error Resume snapshot fault hooks must never enter a product target.
#endif

#include <Windows.h>
#include <cstdint>

namespace pbapp::test::snapshot
{
enum class Fault
{
    None,
    WriteFailure,
    ShortWrite,
    FlushFailure,
    ReplaceFailure,
    ReopenFailure,
    CrashAfterTemporaryFlush,
    CrashAfterReplace
};

struct State
{
    Fault fault = Fault::None;
    std::uint64_t writes = 0;
    std::uint64_t flushes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t reopens = 0;
};

// Only private test translation units share this thread-local selection.
inline thread_local State* activeState = nullptr;

class ScopedState
{
public:
    explicit ScopedState(State& state) noexcept : previous_(activeState)
    {
        activeState = &state;
    }
    ~ScopedState()
    {
        activeState = previous_;
    }
    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;
private:
    State* previous_;
};

[[noreturn]] inline void CrashNow() noexcept
{
    // Real process termination, not an exception: bypasses destructors and checkpoint.
    TerminateProcess(GetCurrentProcess(), 219);
    ExitProcess(220);
}

inline BOOL WriteSnapshot(const HANDLE file, const void* bytes, const DWORD count, DWORD* written) noexcept
{
    if (activeState != nullptr)
    {
        activeState->writes++;
        if (activeState->fault == Fault::WriteFailure)
        {
            *written = 0;
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        if (activeState->fault == Fault::ShortWrite)
        {
            return WriteFile(file, bytes, count > 37 ? 37 : 0, written, nullptr);
        }
    }
    return WriteFile(file, bytes, count, written, nullptr);
}

inline BOOL FlushSnapshot(const HANDLE file) noexcept
{
    if (activeState != nullptr)
    {
        activeState->flushes++;
        if (activeState->fault == Fault::FlushFailure)
        {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
    }
    const BOOL flushed = FlushFileBuffers(file);
    if (flushed && activeState != nullptr && activeState->fault == Fault::CrashAfterTemporaryFlush)
    {
        CrashNow();
    }
    return flushed;
}

inline BOOL ReplaceSnapshot(const wchar_t* source, const wchar_t* destination, const DWORD flags) noexcept
{
    if (activeState != nullptr)
    {
        activeState->replaces++;
        if (activeState->fault == Fault::ReplaceFailure)
        {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
    }
    const BOOL replaced = MoveFileExW(source, destination, flags);
    if (replaced && activeState != nullptr && activeState->fault == Fault::CrashAfterReplace)
    {
        CrashNow();
    }
    return replaced;
}

inline HANDLE ReopenSnapshot(const wchar_t* path) noexcept
{
    if (activeState != nullptr)
    {
        activeState->reopens++;
        if (activeState->fault == Fault::ReopenFailure)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}
}
