#pragma once

#include "feedback_protocol.h"

#include <memory>

namespace pbapp
{

[[nodiscard]] FeedbackStatus ResolveFeedbackFilePath(const FeedbackConfig& config, std::filesystem::path& output) noexcept;
// Synchronous by design: product callers must run this in the isolated helper.
[[nodiscard]] FeedbackStatus ReadFeedbackFile(const std::filesystem::path& path, std::vector<std::byte>& output) noexcept;

class FeedbackFilePublisher
{
public:
    FeedbackFilePublisher();
    ~FeedbackFilePublisher();
    FeedbackFilePublisher(const FeedbackFilePublisher&) = delete;
    FeedbackFilePublisher& operator=(const FeedbackFilePublisher&) = delete;
    FeedbackFilePublisher(FeedbackFilePublisher&&) noexcept;
    FeedbackFilePublisher& operator=(FeedbackFilePublisher&&) noexcept;

    [[nodiscard]] FeedbackStatus Open(const std::filesystem::path& path) noexcept;
    [[nodiscard]] FeedbackStatus Publish(const FeedbackSnapshot& snapshot) noexcept;
    [[nodiscard]] std::array<std::byte, 16> GetReceiverId() const noexcept;
    void Close() noexcept;

private:
    friend class FeedbackFilePublisherTestAccess;
    [[nodiscard]] FeedbackStatus PublishInternal(const FeedbackSnapshot& snapshot, void (*beforeCommit)(void*), void* context) noexcept;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
