#include "feedback_io_channel.h"
#include <iostream>
#include <string_view>

int wmain(const int argumentCount, wchar_t** const arguments)
{
    try
    {
        if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--build-identity")
        {
            std::cout << "{\"application\":\"PBFeedbackIo\",\"feedbackProtocol\":1,\"ipcProtocol\":1,\"metadataOnly\":true}\n";
            return 0;
        }
        return pbapp::detail::RunFeedbackIoWorker(argumentCount, arguments);
    }
    catch (...)
    {
        return 9;
    }
}
