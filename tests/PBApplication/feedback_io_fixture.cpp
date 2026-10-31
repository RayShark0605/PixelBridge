#include "feedback_io_channel.h"
#include <string>

int wmain(const int argumentCount, wchar_t** const arguments)
{
    pbapp::detail::FeedbackIoLaunch launch;
    if (!pbapp::detail::ParseFeedbackIoLaunch(argumentCount, arguments, launch))
    {
        return 2;
    }
    const auto mode = launch.path.filename().native();
    if (mode == L"exit.pbfb")
    {
        return 0;
    }
    if (mode == L"blocked.pbfb")
    {
        Sleep(INFINITE);
        return 0;
    }
    pbapp::detail::FeedbackIoMessage message;
    message.type = pbapp::detail::FeedbackIoMessageType::Ready;
    message.runToken = launch.runToken;
    message.nonce = launch.nonce;
    message.body.resize(17);
    if (mode == L"blocked-operation.pbfb")
    {
        message.body.assign(17, std::byte{1});
        message.body[0] = launch.writer ? std::byte{1} : std::byte{0};
        if (!pbapp::detail::WriteFeedbackIoMessage(launch.pipe, message))
        {
            return 5;
        }
        Sleep(INFINITE);
        return 0;
    }
    std::vector<std::byte> bytes;
    if (!pbapp::detail::EncodeFeedbackIoMessage(message, bytes))
    {
        return 3;
    }
    if (mode == L"bad-version.pbfb")
    {
        bytes[4] = std::byte{2};
    }
    else if (mode == L"bad-token.pbfb")
    {
        bytes[24] ^= std::byte{1};
    }
    else if (mode == L"bad-length.pbfb")
    {
        bytes[10] = std::byte{2};
    }
    DWORD written = 0;
    if (!WriteFile(launch.pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
    {
        return 4;
    }
    Sleep(INFINITE);
    return 0;
}
