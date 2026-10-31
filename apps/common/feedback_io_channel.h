#pragma once

#include "feedback_protocol.h"
#include <Windows.h>
#include <array>
#include <span>
#include <vector>

namespace pbapp::detail
{
inline constexpr std::size_t feedbackIoHeaderBytes = 48;
inline constexpr std::size_t maximumFeedbackIoMessageBytes = feedbackIoHeaderBytes + maximumFeedbackSnapshotBytes;

enum class FeedbackIoMessageType : std::uint16_t
{
    Ready = 1,
    Snapshot = 2,
    WriteSnapshot = 3,
    Error = 4,
    Published = 5
};

struct FeedbackIoMessage
{
    FeedbackIoMessageType type = FeedbackIoMessageType::Error;
    FeedbackError error = FeedbackError::None;
    std::uint32_t nativeError = 0;
    std::uint64_t runToken = 0;
    std::array<std::byte, 16> nonce{};
    std::vector<std::byte> body;
};

struct FeedbackIoLaunch
{
    bool writer = false;
    std::filesystem::path path;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::uint64_t runToken = 0;
    std::array<std::byte, 16> nonce{};
};

[[nodiscard]] FeedbackStatus EncodeFeedbackIoMessage(const FeedbackIoMessage& message, std::vector<std::byte>& output);
// A header is enough to validate the length before buffering a body.
[[nodiscard]] FeedbackStatus GetFeedbackIoMessageSize(std::span<const std::byte> header, std::size_t& output) noexcept;
[[nodiscard]] FeedbackStatus DecodeFeedbackIoMessage(std::span<const std::byte> bytes, FeedbackIoMessage& output);
[[nodiscard]] bool ParseFeedbackIoLaunch(int argumentCount, wchar_t** arguments, FeedbackIoLaunch& output);
[[nodiscard]] bool WriteFeedbackIoMessage(HANDLE pipe, const FeedbackIoMessage& message);
int RunFeedbackIoWorker(int argumentCount, wchar_t** arguments);
}
