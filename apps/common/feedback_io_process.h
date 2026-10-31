#pragma once

#include "feedback_protocol.h"
#include <memory>
#include <optional>

namespace pbapp
{
enum class FeedbackIoRole : std::uint8_t
{
    Reader,
    Writer
};

struct FeedbackIoResult
{
    FeedbackStatus status;
    std::optional<FeedbackSnapshot> snapshot;
    std::array<std::byte, 16> receiverId{};
    std::uint64_t runToken = 0;
    std::uint64_t publishedSequence = 0;
    bool running = false;
    bool ready = false;
    bool timedOut = false;
};

// Single-owner polling object. Only the child may touch the selected path.
// Keeping this object across runs prevents an unconfirmed old child being lost.
class FeedbackIoProcess
{
public:
    explicit FeedbackIoProcess(std::filesystem::path trustedHelperPath = {});
    ~FeedbackIoProcess();
    FeedbackIoProcess(const FeedbackIoProcess&) = delete;
    FeedbackIoProcess& operator=(const FeedbackIoProcess&) = delete;
    [[nodiscard]] FeedbackStatus Start(FeedbackIoRole role, const FeedbackConfig& config, std::uint64_t runToken);
    [[nodiscard]] FeedbackStatus QueueSnapshot(const FeedbackSnapshot& snapshot, std::uint64_t runToken);
    [[nodiscard]] FeedbackIoResult Poll(std::uint64_t nowNanoseconds);
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::size_t GetPendingSnapshotCount() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
}
