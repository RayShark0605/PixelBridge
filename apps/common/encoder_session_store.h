#pragma once

#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbapp
{

inline constexpr std::uint64_t encoderDurableIdLeaseSize = 4096;

struct EncoderSourceIdentity
{
    std::uint64_t volumeSerialNumber = 0;
    std::array<std::byte, 16> fileId{};
    std::uint64_t fileBytes = 0;
    std::uint64_t lastWriteTime = 0;

    bool operator==(const EncoderSourceIdentity&) const = default;
};

struct EncoderSessionStoreStatus
{
    bool success = true;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }

    [[nodiscard]] static EncoderSessionStoreStatus Failure(std::string message)
    {
        return {false, std::move(message)};
    }
};

struct EncoderSessionStoreCreateConfig
{
    std::filesystem::path rootDirectory;
    EncoderSourceIdentity sourceIdentity;
    std::string sourcePathUtf8;
    std::string buildIdentity;
    std::string compressionIdentity;
    std::string outerFecIdentity;
    pbprotocol::SessionId sessionId{};
    std::uint64_t segmentCount = 0;
    std::vector<std::byte> descriptorBundle;
};

// Crash-safe owner of one Encoder session's immutable descriptor bundle and
// mutable ID high-water marks. Every lease endpoint is atomically replaced and
// flushed before the corresponding FrameSequence or repair ID may be shown.
class EncoderSessionStore
{
public:
    [[nodiscard]] static EncoderSessionStoreStatus FindMatching(
        const std::filesystem::path& rootDirectory, const EncoderSourceIdentity& sourceIdentity,
        std::string_view buildIdentity, std::string_view compressionIdentity,
        std::string_view outerFecIdentity, std::unique_ptr<EncoderSessionStore>& output,
        bool& found) noexcept;
    [[nodiscard]] static EncoderSessionStoreStatus Create(const EncoderSessionStoreCreateConfig& config,
        std::unique_ptr<EncoderSessionStore>& output) noexcept;
    // Explicit stopped-session deletion, never called by ordinary Stop.
    // Validates the exact SessionId, refuses live owners/reparse points or
    // unexpected files, and removes only an index still pointing at this ID.
    [[nodiscard]] static EncoderSessionStoreStatus EndAndDelete(
        const std::filesystem::path& rootDirectory, std::string_view sessionIdHex) noexcept;

    EncoderSessionStore(const EncoderSessionStore&) = delete;
    EncoderSessionStore& operator=(const EncoderSessionStore&) = delete;
    ~EncoderSessionStore();

    [[nodiscard]] const pbprotocol::SessionId& GetSessionId() const noexcept;
    [[nodiscard]] bool MatchesDescriptorBundle(std::span<const std::byte> descriptorBundle) const noexcept;
    [[nodiscard]] std::uint64_t GetFrameSequenceStart() const noexcept;
    [[nodiscard]] std::uint64_t GetFrameSequenceLeaseEnd() const noexcept;
    [[nodiscard]] std::uint32_t GetRepairIdStart(std::uint64_t segmentOrdinal,
        std::uint32_t systematicBlockCount) const noexcept;
    [[nodiscard]] std::uint32_t GetRepairIdLeaseEnd(std::uint64_t segmentOrdinal) const noexcept;
    [[nodiscard]] std::uint64_t GetCarouselPass() const noexcept;
    [[nodiscard]] std::uint64_t GetSegmentOrdinal() const noexcept;
    [[nodiscard]] std::uint64_t GetGeneration() const noexcept;
    [[nodiscard]] bool WasResumed() const noexcept;
    [[nodiscard]] const std::filesystem::path& GetSessionDirectory() const noexcept;

    [[nodiscard]] EncoderSessionStoreStatus EnsureFrameSequenceLease(std::uint64_t requiredExclusive) noexcept;
    [[nodiscard]] EncoderSessionStoreStatus EnsureRepairIdLease(std::uint64_t segmentOrdinal,
        std::uint64_t requiredExclusive) noexcept;
    [[nodiscard]] EncoderSessionStoreStatus UpdateCarouselPosition(std::uint64_t carouselPass,
        std::uint64_t segmentOrdinal) noexcept;

private:
    struct Implementation;
    explicit EncoderSessionStore(std::unique_ptr<Implementation> implementation) noexcept;
    [[nodiscard]] EncoderSessionStoreStatus PersistRuntimeState() noexcept;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] EncoderSessionStoreStatus ResolveEncoderSessionRoot(
    const std::filesystem::path& configuredRoot, std::filesystem::path& output) noexcept;

} // namespace pbapp
