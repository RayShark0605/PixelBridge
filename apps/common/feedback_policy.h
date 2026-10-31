#pragma once

#include "feedback_protocol.h"

#include <array>
#include <cstdint>
#include <optional>

namespace pbapp
{

inline constexpr std::uint64_t feedbackFreshnessNanoseconds = 15ULL * 1000ULL * 1000ULL * 1000ULL;
inline constexpr std::size_t maximumFeedbackSelectedSegments = 6;

// Owned by the sending run. Neither helper threads nor presentation callbacks
// may mutate this ledger or request stop using its stale view.
class FeedbackSenderPolicy
{
public:
    [[nodiscard]] FeedbackStatus Reset(const FeedbackIdentity& identity, std::uint64_t runToken) noexcept;
    [[nodiscard]] FeedbackStatus RecordSubmittedFrame(std::uint64_t frameSequence) noexcept;
    // Success means canonical matching state was observed; a heartbeat without
    // a newer visual observation does not necessarily restore effective state.
    [[nodiscard]] FeedbackStatus Accept(const FeedbackSnapshot& snapshot, std::uint64_t nowNanoseconds) noexcept;
    [[nodiscard]] bool Expire(std::uint64_t nowNanoseconds) noexcept;
    void MarkUnavailable(FeedbackError error) noexcept;
    [[nodiscard]] bool IsSegmentConfirmed(std::uint64_t segmentOrdinal) const noexcept;
    [[nodiscard]] bool CanStop(std::uint64_t currentRunToken) const noexcept;
    [[nodiscard]] const FeedbackTelemetry& GetTelemetry() const noexcept;
    [[nodiscard]] std::uint32_t GetSegmentCount() const noexcept;

private:
    [[nodiscard]] FeedbackStatus Reject(FeedbackError error, const char* message, bool permanent = false) noexcept;
    [[nodiscard]] bool IsFresh(std::uint64_t nowNanoseconds) const noexcept;
    void ApplyAcceptedState(std::uint64_t nowNanoseconds) noexcept;

    FeedbackIdentity identity_;
    FeedbackTelemetry telemetry_;
    std::optional<FeedbackSnapshot> accepted_;
    std::array<std::array<std::byte, 16>, 8> retiredEpochs_{};
    std::size_t retiredEpochCount_ = 0;
    std::uint64_t runToken_ = 0;
    std::uint64_t firstSubmittedSequence_ = 0;
    std::uint64_t nextSubmittedSequence_ = 0;
    std::uint64_t lastObservedSequence_ = 0;
    std::uint64_t lastFreshObservationAt_ = 0;
    bool hasSubmitted_ = false;
    bool hasFreshObservation_ = false;
    bool conflictLatched_ = false;
};

struct FeedbackSegmentSelection
{
    std::array<std::uint64_t, maximumFeedbackSelectedSegments> segmentOrdinals{};
    std::uint32_t count = 0;
    bool overrideSelection = false;
    bool keepalive = false;
};

[[nodiscard]] FeedbackSegmentSelection SelectFeedbackSegments(const FeedbackSenderPolicy& policy, std::uint64_t& cursor) noexcept;

}
