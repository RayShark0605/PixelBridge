#include "gate_support.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <vector>

namespace screenregiongate
{
namespace
{
class Handle
{
public:
    explicit Handle(const HANDLE value = nullptr) noexcept : value_(value)
    {
    }
    ~Handle()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept
    {
        return value_;
    }

private:
    HANDLE value_;
};

class Child
{
public:
    explicit Child(const std::filesystem::path& executable)
    {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE outputRead = nullptr;
        HANDLE outputWrite = nullptr;
        Require(CreatePipe(&outputRead, &outputWrite, &security, 4096), "stdout pipe creation failed");
        outputRead_ = outputRead;
        const Handle outputWriter(outputWrite);
        HANDLE errorRead = nullptr;
        HANDLE errorWrite = nullptr;
        if (!CreatePipe(&errorRead, &errorWrite, &security, 4096))
        {
            CloseHandle(outputRead_);
            outputRead_ = nullptr;
            throw std::runtime_error("stderr pipe creation failed");
        }
        errorRead_ = errorRead;
        const Handle errorWriter(errorWrite);
        try
        {
            Require(SetHandleInformation(outputRead_, HANDLE_FLAG_INHERIT, 0) && SetHandleInformation(errorRead_, HANDLE_FLAG_INHERIT, 0),
                    "pipe inheritance isolation failed");
            const Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            Require(input.Get() != INVALID_HANDLE_VALUE, "NUL stdin creation failed");
            SIZE_T attributeBytes = 0;
            (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            Require(attributeBytes > 0 && attributeBytes <= 65536, "invalid process attribute size");
            std::vector<std::byte> attributes(attributeBytes);
            auto* const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            Require(InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes), "process attribute initialization failed");
            const std::array<HANDLE, 3> inherited{outputWrite, errorWrite, input.Get()};
            const BOOL updated = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inherited.data()), sizeof(inherited),
                                                           nullptr, nullptr);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input.Get();
            startup.StartupInfo.hStdOutput = outputWrite;
            startup.StartupInfo.hStdError = errorWrite;
            startup.lpAttributeList = list;
            std::wstring command = L"\"" + executable.wstring() + L"\" --select-region";
            PROCESS_INFORMATION process{};
            const BOOL created =
                updated && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                          CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
            const DWORD creationError = GetLastError();
            DeleteProcThreadAttributeList(list);
            Require(created != FALSE, "Decoder launch failed: " + std::to_string(creationError));
            process_ = process.hProcess;
            processId_ = process.dwProcessId;
            const Handle thread(process.hThread);
            (void)AllowSetForegroundWindow(processId_);
            Require(ResumeThread(thread.Get()) != static_cast<DWORD>(-1), "Decoder resume failed");
        }
        catch (...)
        {
            Cleanup();
            throw;
        }
    }
    ~Child()
    {
        Cleanup();
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    HWND FindWindow() const
    {
        struct Context
        {
            DWORD processId;
            HWND result;
        } context{processId_, nullptr};
        EnumWindows(
            [](const HWND window, const LPARAM parameter) -> BOOL
            {
                auto& state = *reinterpret_cast<Context*>(parameter);
                DWORD processId = 0;
                GetWindowThreadProcessId(window, &processId);
                std::array<wchar_t, 128> className{};
                if (processId == state.processId && IsWindowVisible(window) &&
                    GetClassNameW(window, className.data(), static_cast<int>(className.size())) > 0 &&
                    std::wstring_view(className.data()).starts_with(L"PixelBridge.Region."))
                {
                    state.result = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
        return context.result;
    }

    HWND WaitForWindow() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (const HWND window = FindWindow(); window != nullptr)
            {
                return window;
            }
            Require(WaitForSingleObject(process_, 0) == WAIT_TIMEOUT, "Decoder exited before creating the overlay");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Decoder overlay timed out");
    }

    void AwaitTitle(const HWND window, const wchar_t* const suffix) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::array<wchar_t, 160> text{};
            GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
            if (std::wstring_view(text.data()).ends_with(suffix))
            {
                return;
            }
            Require(WaitForSingleObject(process_, 0) == WAIT_TIMEOUT, "Decoder exited before input acknowledgment");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Decoder input acknowledgment timed out");
    }

    DWORD Wait()
    {
        Require(WaitForSingleObject(process_, 15000) == WAIT_OBJECT_0, "Decoder exit timed out");
        DWORD code = 0;
        Require(GetExitCodeProcess(process_, &code) != FALSE, "cannot read Decoder exit code");
        return code;
    }
    std::string Output() const
    {
        return ReadPipe(outputRead_);
    }
    std::string Error() const
    {
        return ReadPipe(errorRead_);
    }

private:
    static std::string ReadPipe(const HANDLE pipe)
    {
        std::string value;
        std::array<char, 512> buffer{};
        DWORD bytes = 0;
        for (;;)
        {
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr))
            {
                Require(GetLastError() == ERROR_BROKEN_PIPE, "Decoder pipe read failed");
                break;
            }
            if (bytes == 0)
            {
                break;
            }
            Require(value.size() + bytes <= 8192, "Decoder output exceeded gate bound");
            value.append(buffer.data(), bytes);
        }
        std::erase(value, '\r');
        return value;
    }
    void Cleanup() noexcept
    {
        if (process_ != nullptr)
        {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
            {
                (void)TerminateProcess(process_, 92);
                (void)WaitForSingleObject(process_, 5000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (outputRead_ != nullptr)
        {
            CloseHandle(outputRead_);
            outputRead_ = nullptr;
        }
        if (errorRead_ != nullptr)
        {
            CloseHandle(errorRead_);
            errorRead_ = nullptr;
        }
    }
    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    HANDLE outputRead_ = nullptr;
    HANDLE errorRead_ = nullptr;
};
} // namespace

void RunCliGate(const std::filesystem::path& decoder, Evidence& evidence)
{
    RestorePointer cursor;
    const auto monitors = ReadOracle();
    for (const bool cancel : {false, true})
    {
        Child child(decoder);
        const HWND window = child.WaitForWindow();
        Require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
                "Decoder's actual HWND is not PMv2");
        const HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        const auto monitor = std::find_if(monitors.begin(), monitors.end(),
                                          [&](const OracleMonitor& entry)
                                          {
                                              return entry.monitor == handle;
                                          });
        Require(monitor != monitors.end(), "Decoder monitor not in independent oracle");
        const RECT rect = SmallRect(*monitor);
        try
        {
            MovePointer({rect.left, rect.top});
            MouseButton(MOUSEEVENTF_LEFTDOWN);
            child.AwaitTitle(window, L"dragging");
            if (cancel)
            {
                Escape();
                MouseButton(MOUSEEVENTF_LEFTUP);
            }
            else
            {
                MovePointer({rect.right - 1, rect.bottom - 1});
                // Down is acknowledged before moving. Up samples the physical
                // cursor itself, so it cannot reuse a queued down position.
                MouseButton(MOUSEEVENTF_LEFTUP);
            }
            const DWORD code = child.Wait();
            const std::string output = child.Output();
            const std::string error = child.Error();
            Require(!IsWindow(window) && child.FindWindow() == nullptr, "Decoder left an overlay behind");
            if (cancel)
            {
                Require(code == 3 && output.empty() && error.find("cancelled") != std::string::npos, "Decoder cancellation contract mismatch");
                evidence.Note("Decoder actual Escape: exit=3, stdout empty, overlays destroyed");
            }
            else
            {
                const ScreenCaptureRegion expected{monitor->monitor, rect, monitor->rect, monitor->dpi, monitor->dpi, monitor->rotation};
                std::ostringstream expectedJson;
                WriteScreenCaptureRegionJson(expectedJson, expected);
                Require(code == 0 && output == expectedJson.str() + '\n' && error.empty(), "Decoder JSON/exit mismatch: " + output + error);
                evidence.Record("decoder-independent-expected", expected);
                std::ofstream file(evidence.Directory() / "decoder-stdout.json");
                file.exceptions(std::ios::badbit | std::ios::failbit);
                file << output;
                file.close();
                evidence.Note("Decoder actual drag: exit=0, exact one-line physical JSON, overlays destroyed");
            }
        }
        catch (...)
        {
            try
            {
                MouseButton(MOUSEEVENTF_LEFTUP);
            }
            catch (...)
            {
            }
            throw;
        }
    }
    cursor.Restore();
    evidence.Note("PASS: public Decoder entry is reachable through real input; no capture was performed");
}

} // namespace screenregiongate
