#include "application_console.h"

#include <memory>
#include <string>
#include <string_view>

namespace
{

struct HandleCloser
{
    void operator()(void* const handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};

using OwnedHandle = std::unique_ptr<void, HandleCloser>;

int RunHiddenConsoleParent(const wchar_t* const childPath, const wchar_t* const redirectedPath = nullptr)
{
    if (GetConsoleCP() == 0 || std::wstring_view(childPath).find(L'"') != std::wstring_view::npos)
    {
        return 10;
    }
    const OwnedHandle console(CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (console.get() == INVALID_HANDLE_VALUE)
    {
        return 11;
    }
    // No inherited STARTF_USESTDHANDLES: exercise the GUI CRT's missing-handle
    // path and real AttachConsole, not only already-valid redirected streams.
    std::wstring command = L"\"" + std::wstring(childPath) + L"\" --console-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    OwnedHandle redirectedOutput;
    OwnedHandle consoleInput;
    if (redirectedPath != nullptr)
    {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        redirectedOutput.reset(CreateFileW(redirectedPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        consoleInput.reset(CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, 0, nullptr));
        if (redirectedOutput.get() == INVALID_HANDLE_VALUE || consoleInput.get() == INVALID_HANDLE_VALUE ||
            !SetHandleInformation(console.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
        {
            return 17;
        }
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = consoleInput.get();
        startup.hStdOutput = redirectedOutput.get();
        startup.hStdError = console.get();
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(childPath, command.data(), nullptr, nullptr, redirectedPath != nullptr, 0, nullptr, nullptr, &startup, &process))
    {
        return 12;
    }
    const OwnedHandle processHandle(process.hProcess);
    const OwnedHandle threadHandle(process.hThread);
    if (WaitForSingleObject(processHandle.get(), 5000) != WAIT_OBJECT_0)
    {
        TerminateProcess(processHandle.get(), 13);
        WaitForSingleObject(processHandle.get(), 5000);
        return 13;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode != 0)
    {
        return 14;
    }
    std::array<wchar_t, 4096> text{};
    DWORD readCharacters = 0;
    if (!ReadConsoleOutputCharacterW(console.get(), text.data(), static_cast<DWORD>(text.size()), {0, 0}, &readCharacters))
    {
        return 15;
    }
    const std::wstring_view captured(text.data(), readCharacters);
    if (captured.find(L"ATTACHED_STDERR=1") == std::wstring_view::npos)
    {
        return 16;
    }
    if (redirectedPath != nullptr)
    {
        std::array<char, 256> output{};
        DWORD bytesRead = 0;
        const LARGE_INTEGER start{};
        if (!SetFilePointerEx(redirectedOutput.get(), start, nullptr, FILE_BEGIN) ||
            !ReadFile(redirectedOutput.get(), output.data(), static_cast<DWORD>(output.size()), &bytesRead, nullptr) ||
            std::string_view(output.data(), bytesRead).find("ATTACHED_STDOUT=1 INPUT_CONSOLE=1") == std::string_view::npos ||
            captured.find(L"ATTACHED_STDOUT=1") != std::wstring_view::npos)
        {
            return 18;
        }
    }
    else if (captured.find(L"ATTACHED_STDOUT=1 INPUT_CONSOLE=1") == std::wstring_view::npos)
    {
        return 19;
    }
    std::cout << "HIDDEN_PARENT_PASS: stdout/stderr and console input handle repaired; no input generated\n";
    return 0;
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--hidden-parent")
    {
        return RunHiddenConsoleParent(arguments[2]);
    }
    if (argumentCount == 4 && std::wstring_view(arguments[1]) == L"--hidden-parent-redirection")
    {
        return RunHiddenConsoleParent(arguments[2], arguments[3]);
    }
    if (argumentCount == 1)
    {
        // Same no-argument branch as the applications: never attach/create.
        std::cout << "GUI_NO_ARGUMENTS_CONSOLE=" << (GetConsoleCP() != 0 ? 1 : 0) << '\n';
        return GetConsoleCP() == 0 ? 0 : 20;
    }
    if (!pbapp::PrepareCommandLineStreams())
    {
        return 21;
    }
    if (std::wstring_view(arguments[1]) == L"--console-child")
    {
        DWORD mode = 0;
        const bool inputConsole = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != FALSE;
        std::cout << "ATTACHED_STDOUT=" << (GetConsoleCP() != 0 ? 1 : 0) << " INPUT_CONSOLE=" << inputConsole << '\n';
        std::cerr << "ATTACHED_STDERR=1\n";
        return inputConsole ? 0 : 22;
    }
    if (std::wstring_view(arguments[1]) == L"--redirect-probe")
    {
        std::string input;
        std::getline(std::cin, input);
        std::cout << "OUT:" << input << '\n';
        std::cerr << "ERR:" << input << '\n';
        return std::cin.bad() || !std::cout.good() || !std::cerr.good() ? 23 : 0;
    }
    return 24;
}
