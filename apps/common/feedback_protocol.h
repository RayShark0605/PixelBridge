#pragma once

#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pbapp
{

inline constexpr std::size_t maximumFeedbackSnapshotBytes = 64U * 1024U;
inline constexpr std::uint32_t maximumFeedbackSegmentCount = 65536;
inline constexpr std::uint64_t maximumFeedbackFileBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;

enum class FeedbackError : std::uint8_t
{
    None,
    InvalidFormat,
    UnsupportedVersion,
    LimitExceeded,
    IntegrityMismatch,
    InconsistentState,
    AllocationFailure,
    InvalidPath,
    IoFailure,
    Unavailable,
    Conflict,
    Stale,
    Superseded
};

struct FeedbackStatus
{
    FeedbackError error = FeedbackError::None;
    const char* message = "";
    std::uint32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == FeedbackError::None;
    }
};

enum class FeedbackLocationKind : std::uint8_t
{
    Directory,
    File
};

struct FeedbackConfig
{
    std::filesystem::path path;
    FeedbackLocationKind locationKind = FeedbackLocationKind::Directory;
};

enum class FeedbackPhase : std::uint8_t
{
    Waiting = 1,
    Receiving = 2,
    Verifying = 3,
    Completed = 4,
    Failed = 5
};

struct FeedbackIdentity
{
    pbprotocol::SessionId sessionId{};
    std::uint64_t profileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t segmentCount = 0;
    std::array<std::byte, 32> canonicalSessionDigest{};
    std::array<std::byte, 32> canonicalManifestDigest{};
    pbprotocol::WholeFileDigest wholeFileDigest{};

    bool operator==(const FeedbackIdentity&) const = default;
};

struct FeedbackSnapshot
{
    FeedbackIdentity identity;
    std::array<std::byte, 16> receiverId{};
    std::array<std::byte, 16> receiverEpoch{};
    std::uint64_t snapshotSequence = 0;
    std::uint64_t observedFrameSequence = 0;
    std::uint64_t reportedCompletionMilliseconds = 0;
    std::vector<std::byte> durableCompleted;
    FeedbackPhase phase = FeedbackPhase::Waiting;
    bool identityBound = false;
    bool observedFrameAvailable = false;
    bool wholeDigestVerified = false;
    bool finalPublishSucceeded = false;
    bool finalReopenVerified = false;

    bool operator==(const FeedbackSnapshot&) const = default;
};

enum class FeedbackConnectionState : std::uint8_t
{
    Disabled,
    Waiting,
    Active,
    Stale,
    Unavailable,
    Rejected,
    ReceiverReportedComplete
};

struct FeedbackTelemetry
{
    bool requested = false;
    bool effective = false;
    FeedbackConnectionState state = FeedbackConnectionState::Disabled;
    FeedbackError lastError = FeedbackError::None;
    std::uint64_t acceptedSnapshots = 0;
    std::uint64_t rejectedSnapshots = 0;
    std::uint64_t duplicateSnapshots = 0;
    std::uint64_t staleEvents = 0;
    std::uint64_t ioTimeouts = 0;
    std::uint64_t confirmedSegments = 0;
    std::uint64_t skippedService = 0;
    std::uint64_t observedFrameSequence = 0;
    bool observedFrameAvailable = false;
    bool terminalPublished = false;
    bool terminalAccepted = false;
    bool listening = false;
    std::optional<std::uint64_t> receiverReportedCompletionMilliseconds;
    std::optional<std::uint64_t> listenerStartedUnixMilliseconds;
    std::optional<std::uint64_t> listenerStoppedUnixMilliseconds;
    std::optional<std::uint64_t> listenerElapsedMilliseconds;
    std::string statusMessage;
};

[[nodiscard]] FeedbackStatus ValidateFeedbackSnapshot(const FeedbackSnapshot& snapshot) noexcept;
[[nodiscard]] FeedbackStatus SerializeFeedbackSnapshot(const FeedbackSnapshot& snapshot, std::vector<std::byte>& output) noexcept;
[[nodiscard]] FeedbackStatus ParseFeedbackSnapshot(std::span<const std::byte> input, FeedbackSnapshot& output) noexcept;
[[nodiscard]] std::uint32_t GetFeedbackCompletedSegmentCount(const FeedbackSnapshot& snapshot) noexcept;
[[nodiscard]] FeedbackStatus BuildFeedbackIdentity(const pbprotocol::SessionDescriptor& session,
    const pbprotocol::FinalManifest& manifest, std::uint8_t layoutVersion,
    const pbprotocol::ReceiverResourcePolicy& policy, FeedbackIdentity& output) noexcept;

}
