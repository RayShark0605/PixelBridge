#include "gate_support.h"

#include <array>

namespace screenregiongate
{
namespace
{
class Worker
{
public:
    explicit Worker(const std::filesystem::path& evidence)
    {
        try
        {
            job_ = CreateJobObjectW(nullptr, nullptr);
            Require(job_ != nullptr, "native supervisor job creation failed");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            Require(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "native supervisor job setup failed");
            std::array<wchar_t, 32768> executable{};
            const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            Require(length != 0 && length < executable.size(), "native supervisor executable path unavailable");
            std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" --native-worker \"" + evidence.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION information{};
            Require(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                   &information),
                    "native supervisor worker launch failed");
            process_ = information.hProcess;
            thread_ = information.hThread;
            Require(AssignProcessToJobObject(job_, process_), "native supervisor could not isolate worker");
            (void)AllowSetForegroundWindow(information.dwProcessId);
            Require(ResumeThread(thread_) != static_cast<DWORD>(-1), "native supervisor worker resume failed");
        }
        catch (...)
        {
            Stop();
            throw;
        }
    }
    ~Worker()
    {
        Stop();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    DWORD Wait() const
    {
        Require(WaitForSingleObject(process_, 150000) == WAIT_OBJECT_0, "native worker timed out or wait failed");
        DWORD code = 0;
        Require(GetExitCodeProcess(process_, &code), "native worker exit code unavailable");
        return code;
    }

    void Stop() noexcept
    {
        if (process_ != nullptr)
        {
            if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0)
            {
                // Covers a suspended worker for which job assignment failed too.
                (void)TerminateProcess(process_, 93);
                (void)WaitForSingleObject(process_, 5000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_ != nullptr)
        {
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (job_ != nullptr)
        {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }

private:
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
};

void ReleaseTestInput()
{
    MouseButton(MOUSEEVENTF_LEFTUP);
    MouseButton(MOUSEEVENTF_RIGHTUP);
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_ESCAPE;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    Require(SendInput(1, &input, sizeof(input)) == 1, "native supervisor could not release test key");
}
} // namespace

void RunSupervisedNativeGate(const std::filesystem::path& evidenceRoot)
{
    Require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0,
            "BLOCKED: desktop gate cannot run during user input");
    RestorePointer cursor;
    Evidence evidence(evidenceRoot, "supervisor");
    Worker worker(evidence.Directory());
    try
    {
        const DWORD code = worker.Wait();
        worker.Stop();
        ReleaseTestInput();
        cursor.Restore();
        evidence.Note("worker-exit=" + std::to_string(code) + "; parent independently restored physical cursor and released test input");
        Require(code == 0, "native worker failed (not skipped): " + std::to_string(code));
    }
    catch (...)
    {
        worker.Stop();
        ReleaseTestInput();
        cursor.Restore();
        throw;
    }
}
} // namespace screenregiongate
