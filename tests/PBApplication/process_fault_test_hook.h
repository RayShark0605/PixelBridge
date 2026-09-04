#pragma once

#ifndef PB_PROCESS_FAULT_TESTS
#error Process fault hooks are restricted to the opt-in test libraries.
#endif

#include <Windows.h>
#include <psapi.h>
#include <RestartManager.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>

namespace pbapp::test
{

inline constexpr unsigned long processCrashExitCode = 218;

[[nodiscard]] inline std::wstring ReadEnvironment(const wchar_t* const name)
{
    std::array<wchar_t, 4096> buffer{};
    const DWORD characters = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (characters >= buffer.size())
    {
        throw std::runtime_error("G18 test environment value exceeds its bound");
    }
    return std::wstring(buffer.data(), characters);
}

[[nodiscard]] inline const std::wstring& SelectedCrashPoint()
{
    static const std::wstring point = ReadEnvironment(L"PB_G18_CRASH_POINT");
    return point;
}

[[nodiscard]] inline PROCESS_MEMORY_COUNTERS ReadProcessMemory()
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE)
    {
        throw std::runtime_error("GetProcessMemoryInfo failed in G18 test worker");
    }
    return counters;
}

// Metadata only. No serialized Control, Transport, source bytes or decoded
// payload ever cross this observation channel. CREATE_NEW preserves evidence.
inline void WriteEvidence(const wchar_t* const name, const std::string_view text)
{
    const std::filesystem::path directory = ReadEnvironment(L"PB_G18_EVIDENCE_DIRECTORY");
    if (directory.empty() || text.size() > 1024ULL * 1024ULL)
    {
        throw std::runtime_error("G18 evidence directory or metadata size is invalid");
    }
    const HANDLE file = CreateFileW((directory / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error("G18 create-only evidence file failed");
    }
    DWORD writtenBytes = 0;
    const bool written = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &writtenBytes, nullptr) != FALSE &&
        writtenBytes == text.size() && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!written || !closed)
    {
        throw std::runtime_error("G18 metadata write/flush/close failed");
    }
}

inline void ObserveRestore(const std::size_t completedSegments, const std::size_t activeBlocks,
    const bool truncatedTail)
{
    WriteEvidence(L"restore.json", "{\"completedSegments\":" + std::to_string(completedSegments) +
        ",\"activeBlocks\":" + std::to_string(activeBlocks) + ",\"truncatedTail\":" +
        (truncatedTail ? "true" : "false") + "}\n");
}

// First retry event per child only; CREATE_NEW never overwrites it. Observe
// after the retry loop so file I/O and Restart Manager cannot cause recovery.
inline void ObserveAtomicReplaceRetry(const std::filesystem::path& targetPath, const DWORD firstError,
    const DWORD finalError, const std::uint32_t attempts, const std::uint64_t elapsedMilliseconds) noexcept
{
    try
    {
        const std::string target = targetPath.filename().string();
        if (target != "runtime.state" && target != "descriptors.bin")
        {
            return;
        }
        WriteEvidence(L"atomic-replace-first-retry.json", "{\"targetFile\":\"" + target +
            "\",\"firstError\":" + std::to_string(firstError) + ",\"finalError\":" + std::to_string(finalError) +
            ",\"attempts\":" + std::to_string(attempts) + ",\"elapsedMilliseconds\":" +
            std::to_string(elapsedMilliseconds) + "}\n");
    }
    catch (...)
    {
        // Missing metadata cannot change persistence success or failure.
    }
}

inline void ObserveAtomicReplaceFailure(const std::filesystem::path& targetPath,
    const std::filesystem::path& temporaryPath, const DWORD originalError) noexcept
{
    try
    {
        const auto ProbeDeleteAccess = [](const std::filesystem::path& path)
        {
            const HANDLE handle = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            const DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
            return error;
        };
        const DWORD targetAttributes = GetFileAttributesW(targetPath.c_str());
        const DWORD temporaryAttributes = GetFileAttributesW(temporaryPath.c_str());
        const DWORD targetDeleteAccess = ProbeDeleteAccess(targetPath);
        const DWORD temporaryDeleteAccess = ProbeDeleteAccess(temporaryPath);
        DWORD session = 0;
        std::array<wchar_t, CCH_RM_SESSION_KEY + 1> key{};
        const DWORD startStatus = RmStartSession(&session, 0, key.data());
        DWORD registerStatus = ERROR_INVALID_HANDLE;
        DWORD listStatus = ERROR_INVALID_HANDLE;
        UINT needed = 0;
        UINT count = 16;
        DWORD rebootReasons = 0;
        std::array<RM_PROCESS_INFO, 16> processes{};
        if (startStatus == ERROR_SUCCESS)
        {
            // The Win32 API accepts a writable pointer array, although the
            // referenced path characters remain const.
            std::array<LPCWSTR, 2> paths{targetPath.c_str(), temporaryPath.c_str()};
            registerStatus = RmRegisterResources(session, static_cast<UINT>(paths.size()), paths.data(), 0, nullptr, 0, nullptr);
            if (registerStatus == ERROR_SUCCESS)
            {
                listStatus = RmGetList(session, &needed, &count, processes.data(), &rebootReasons);
            }
            RmEndSession(session);
        }
        std::ostringstream evidence;
        evidence << "{\"moveError\":" << originalError << ",\"targetAttributes\":" << targetAttributes
            << ",\"temporaryAttributes\":" << temporaryAttributes << ",\"targetDeleteAccessError\":" << targetDeleteAccess
            << ",\"temporaryDeleteAccessError\":" << temporaryDeleteAccess << ",\"restartManagerStart\":" << startStatus
            << ",\"restartManagerRegister\":" << registerStatus << ",\"restartManagerList\":" << listStatus
            << ",\"neededProcesses\":" << needed << ",\"lockingPids\":[";
        if (listStatus == ERROR_SUCCESS)
        {
            for (UINT index = 0; index < count && index < processes.size(); index++)
            {
                evidence << (index == 0 ? "" : ",") << processes[index].Process.dwProcessId;
            }
        }
        evidence << "]}\n";
        WriteEvidence(L"atomic-replace-failure.json", evidence.str());
    }
    catch (...)
    {
        // Observability never replaces the original fail-closed error.
    }
}

// TerminateProcess bypasses unwinding, destructors and the normal checkpoint
// shutdown path. An expected exit code without the durable marker is failure.
inline void CrashAt(const wchar_t* const name, const std::uint64_t value = 0) noexcept
{
    try
    {
        if (SelectedCrashPoint() != name)
        {
            return;
        }
        const PROCESS_MEMORY_COUNTERS counters = ReadProcessMemory();
        const std::wstring wideName(name);
        std::string point;
        point.reserve(wideName.size());
        for (const wchar_t character : wideName)
        {
            if (character < L'a' || character > L'z')
            {
                if (character != L'-')
                {
                    throw std::runtime_error("G18 crash point must use lowercase ASCII identifiers");
                }
            }
            point.push_back(static_cast<char>(character));
        }
        WriteEvidence(L"crash.json", "{\"point\":\"" + point + "\",\"value\":" + std::to_string(value) +
            ",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"workingSetBytes\":" +
            std::to_string(counters.WorkingSetSize) + ",\"peakWorkingSetBytes\":" +
            std::to_string(counters.PeakWorkingSetSize) + "}\n");
        if (TerminateProcess(GetCurrentProcess(), processCrashExitCode) == FALSE)
        {
            std::abort();
        }
        static_cast<void>(WaitForSingleObject(GetCurrentProcess(), INFINITE));
        std::abort();
    }
    catch (...)
    {
        // Never misclassify an instrumentation error as a successful injection.
        std::_Exit(219);
    }
}

} // namespace pbapp::test
