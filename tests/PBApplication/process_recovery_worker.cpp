#include "process_fault_test_hook.h"
#include "process_recovery_test_access.h"

#include <iostream>
#include <string_view>

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount != 6)
    {
        std::cerr << "Usage: PBProcessRecoveryWorker <combined|sender|quota-seed|large-file> <source> <sender-state> <output> <evidence>\n";
        return 2;
    }
    const std::wstring_view modeName(arguments[1]);
    if (modeName != L"combined" && modeName != L"sender" && modeName != L"quota-seed" && modeName != L"large-file")
    {
        return 2;
    }
    if (SetEnvironmentVariableW(L"PB_G18_EVIDENCE_DIRECTORY", arguments[5]) == FALSE)
    {
        return 2;
    }
    const auto mode = modeName == L"sender" ? pbapp::test::ProcessRecoveryMode::SenderOnly :
        modeName == L"quota-seed" ? pbapp::test::ProcessRecoveryMode::QuotaSeed :
        modeName == L"large-file" ? pbapp::test::ProcessRecoveryMode::LargeFile : pbapp::test::ProcessRecoveryMode::Combined;
    return pbapp::test::RunProcessRecoveryWorker(arguments[2], arguments[3], arguments[4], mode);
}
