#include "local_desktop_runtime.h"

#include <Windows.h>
#include <iostream>
#include <string_view>

int wmain(const int argumentCount, wchar_t *arguments[])
{
    if (argumentCount != 5 || std::wstring_view(arguments[1]) != L"--source" || std::wstring_view(arguments[3]) != L"--output")
    {
        std::cerr << "Usage: PBStep1SourceAudit --source <immutable file <=64MiB> --output <new JSON path>\n";
        return 2;
    }
    std::string ledger;
    const auto result = pbapp::AuditUnifiedSource(arguments[2], ledger);
    if (!result)
    {
        std::cerr << result.message << '\n';
        return 1;
    }
    ledger += '\n';
    if (ledger.size() > 65536)
    {
        std::cerr << "Ledger exceeds record limit\n";
        return 1;
    }
    const HANDLE file = CreateFileW(arguments[4], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Output must be a new local path: " << GetLastError() << '\n';
        return 1;
    }
    DWORD written = 0;
    const bool success = WriteFile(file, ledger.data(), static_cast<DWORD>(ledger.size()), &written, nullptr) != FALSE &&
                         written == ledger.size() && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!success || !closed)
    {
        std::cerr << "Ledger write failed; partial evidence retained\n";
        return 1;
    }
    std::cout << ledger;
    return 0;
}
