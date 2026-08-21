#pragma once

#include "pbprotocol/descriptor_codec.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace pbprotocol {

enum class DescriptorBindDisposition : std::uint8_t
{
    Inserted,
    Repeated
};

// DescriptorBindingState has one owning thread. Callers must synchronize any
// cross-thread access; the class itself contains no hidden locks or globals.
class DescriptorBindingState
{
public:
    [[nodiscard]] static ProtocolResult<DescriptorBindingState> Create(
        SessionDescriptor sessionDescriptor,
        ReceiverResourcePolicy resourcePolicy);

    DescriptorBindingState(const DescriptorBindingState&) = delete;
    DescriptorBindingState& operator=(const DescriptorBindingState&) = delete;
    DescriptorBindingState(DescriptorBindingState&&) noexcept = default;
    DescriptorBindingState& operator=(DescriptorBindingState&&) noexcept = default;

    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindSessionDescriptor(
        const SessionDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindSegmentDescriptor(
        const SegmentDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindFinalManifest(
        const FinalManifest& finalManifest);

    [[nodiscard]] ProtocolStatus ValidateCompleteSegmentMap();
    [[nodiscard]] ProtocolStatus ValidateReadyForFinalVerification();
    [[nodiscard]] ProtocolStatus VerifyWholeFileDigest(
        const WholeFileDigest& computedDigest);

    [[nodiscard]] const SessionDescriptor& GetSessionDescriptor() const noexcept;
    [[nodiscard]] std::size_t BoundSegmentCount() const noexcept;
    [[nodiscard]] bool HasFinalManifest() const noexcept;
    [[nodiscard]] bool HasTerminalError() const noexcept;
    [[nodiscard]] ProtocolErrorCode TerminalError() const noexcept;

private:
    DescriptorBindingState(
        SessionDescriptor sessionDescriptor,
        ReceiverResourcePolicy resourcePolicy,
        std::size_t segmentCount) noexcept;

    [[nodiscard]] ProtocolStatus CheckTerminalState() const noexcept;
    [[nodiscard]] ProtocolStatus LatchTerminalError(
        ProtocolErrorCode errorCode,
        std::size_t errorOffset) noexcept;

    SessionDescriptor sessionDescriptor_;
    ReceiverResourcePolicy resourcePolicy_;
    std::size_t segmentCount_ = 0;
    std::map<std::uint64_t, SegmentDescriptor> segmentsByOrdinal_;
    std::map<std::uint64_t, std::uint64_t> ordinalsByRawOffset_;
    std::optional<FinalManifest> finalManifest_;
    ProtocolError terminalError_{};
};

} // namespace pbprotocol
