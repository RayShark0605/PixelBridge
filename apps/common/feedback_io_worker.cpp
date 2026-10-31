#include "feedback_io_channel.h"
#include "feedback_file_store.h"
#include "feedback_win32.h"

namespace pbapp::detail
{
namespace
{
bool ReadExact(const HANDLE pipe, const std::span<std::byte> bytes)
{
    std::size_t position = 0;
    while (position < bytes.size())
    {
        DWORD received = 0;
        if (!ReadFile(pipe, bytes.data() + position, static_cast<DWORD>(bytes.size() - position), &received, nullptr) || received == 0)
        {
            return false;
        }
        position += received;
    }
    return true;
}
}

int RunFeedbackIoWorker(const int argumentCount, wchar_t** const arguments)
{
    FeedbackIoLaunch launch;
    if (!ParseFeedbackIoLaunch(argumentCount, arguments, launch))
    {
        return 2;
    }
    FeedbackHandle pipe(launch.pipe);
    FeedbackFilePublisher publisher;
    FeedbackIoMessage reply;
    reply.runToken = launch.runToken;
    reply.nonce = launch.nonce;
    if (launch.writer)
    {
        const auto opened = publisher.Open(launch.path);
        if (!opened)
        {
            reply.type = FeedbackIoMessageType::Error;
            reply.error = opened.error;
            reply.nativeError = opened.nativeError;
            static_cast<void>(WriteFeedbackIoMessage(pipe.Get(), reply));
            return 3;
        }
    }
    reply.type = FeedbackIoMessageType::Ready;
    reply.body.push_back(launch.writer ? std::byte{1} : std::byte{0});
    const auto receiverId = publisher.GetReceiverId();
    reply.body.insert(reply.body.end(), receiverId.begin(), receiverId.end());
    if (!WriteFeedbackIoMessage(pipe.Get(), reply))
    {
        return 4;
    }
    while (true)
    {
        reply.body.clear();
        FeedbackStatus status;
        if (launch.writer)
        {
            std::vector<std::byte> bytes(feedbackIoHeaderBytes);
            if (!ReadExact(pipe.Get(), bytes))
            {
                return 0;
            }
            std::size_t size = 0;
            if (!GetFeedbackIoMessageSize(bytes, size))
            {
                return 5;
            }
            bytes.resize(size);
            if (!ReadExact(pipe.Get(), std::span(bytes).subspan(feedbackIoHeaderBytes)))
            {
                return 0;
            }
            FeedbackIoMessage request;
            if (!DecodeFeedbackIoMessage(bytes, request) || request.runToken != launch.runToken || request.nonce != launch.nonce ||
                request.type != FeedbackIoMessageType::WriteSnapshot || request.error != FeedbackError::None)
            {
                return 6;
            }
            FeedbackSnapshot snapshot;
            status = ParseFeedbackSnapshot(request.body, snapshot);
            if (status)
            {
                status = publisher.Publish(snapshot);
            }
            reply.type = FeedbackIoMessageType::Published;
            if (status)
            {
                reply.body.resize(8);
                for (std::size_t index = 0; index < 8; index++)
                {
                    reply.body[index] = static_cast<std::byte>((snapshot.snapshotSequence >> (index * 8U)) & 0xFFU);
                }
            }
        }
        else
        {
            status = ReadFeedbackFile(launch.path, reply.body);
            reply.type = FeedbackIoMessageType::Snapshot;
        }
        reply.error = status.error;
        reply.nativeError = status.nativeError;
        if (!status)
        {
            reply.type = FeedbackIoMessageType::Error;
            reply.body.clear();
        }
        if (!WriteFeedbackIoMessage(pipe.Get(), reply))
        {
            return 0;
        }
        if (!launch.writer)
        {
            Sleep(1000);
        }
    }
}
}
