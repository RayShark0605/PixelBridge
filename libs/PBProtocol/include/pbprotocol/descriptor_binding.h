#pragma once

#include "pbprotocol/descriptor_codec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

namespace pbprotocol {

class ControlPlaneReceiver;

namespace detail {

struct DescriptorBindingStorage;

} // namespace detail

namespace test {

class DescriptorBindingStateTestAccess;

} // namespace test

enum class DescriptorBindDisposition : std::uint8_t
{
    Inserted,
    Repeated
};

// Immutable proof that a SegmentDescriptor was admitted into the unique
// SessionTag binding owned by ControlPlaneReceiver. The descriptor is held by
// value so session removal cannot leave a dangling reference. This is a local
// capability and is never serialized.
class BoundSegmentDescriptor
{
public:
    BoundSegmentDescriptor(const BoundSegmentDescriptor&) = default;
    BoundSegmentDescriptor& operator=(const BoundSegmentDescriptor&) = default;
    BoundSegmentDescriptor(BoundSegmentDescriptor&&) noexcept = default;
    BoundSegmentDescriptor& operator=(BoundSegmentDescriptor&&) noexcept =
        default;

    [[nodiscard]] const SegmentDescriptor& GetDescriptor() const noexcept;

    bool operator==(const BoundSegmentDescriptor&) const = default;

private:
    friend class DescriptorBindingState;

    explicit BoundSegmentDescriptor(SegmentDescriptor descriptor) noexcept;

    SegmentDescriptor descriptor_;
};

// DescriptorBindingState has one owning thread. Callers must synchronize any
// cross-thread access; the class itself contains no hidden locks or globals.
class DescriptorBindingState
{
public:
    [[nodiscard]] static ProtocolResult<DescriptorBindingState> Create(
        SessionDescriptor sessionDescriptor,
        ReceiverResourcePolicy resourcePolicy);

    // The state owns a shared reference to upstreamMemoryResource. Its own
    // bounded resource remains authoritative, so a custom arena cannot bypass
    // maxDescriptorStateBytes. This overload is also the allocator fault-
    // injection seam used by resource-path tests.
    [[nodiscard]] static ProtocolResult<DescriptorBindingState>
    CreateWithMemoryResource(
        SessionDescriptor sessionDescriptor,
        ReceiverResourcePolicy resourcePolicy,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource);

    DescriptorBindingState(const DescriptorBindingState&) = delete;
    DescriptorBindingState& operator=(const DescriptorBindingState&) = delete;
    ~DescriptorBindingState();
    DescriptorBindingState(DescriptorBindingState&& other) noexcept;
    DescriptorBindingState& operator=(DescriptorBindingState&&) noexcept = delete;

    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindSessionDescriptor(
        const SessionDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindSegmentDescriptor(
        const SegmentDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindFinalManifest(
        const FinalManifest& finalManifest);

    [[nodiscard]] ProtocolStatus ValidateCompleteSegmentMap();

    [[nodiscard]] const SessionDescriptor& GetSessionDescriptor() const noexcept;
    [[nodiscard]] std::size_t BoundSegmentCount() const noexcept;
    [[nodiscard]] std::uint64_t DescriptorStateBytesInUse() const noexcept;
    [[nodiscard]] bool HasFinalManifest() const noexcept;
    [[nodiscard]] bool HasTerminalError() const noexcept;
    [[nodiscard]] ProtocolErrorCode TerminalError() const noexcept;

private:
    friend class ControlPlaneReceiver;
    friend class test::DescriptorBindingStateTestAccess;

    DescriptorBindingState(
        SessionDescriptor sessionDescriptor,
        ReceiverResourcePolicy resourcePolicy,
        std::size_t segmentCount,
        std::unique_ptr<detail::DescriptorBindingStorage> descriptorStorage) noexcept;

    [[nodiscard]] ProtocolStatus CheckTerminalState() const noexcept;
    [[nodiscard]] ProtocolResult<BoundSegmentDescriptor>
    GetBoundSegmentDescriptor(std::uint64_t segmentOrdinal) const;
    [[nodiscard]] ProtocolResult<bool> IsSegmentCompleted(
        std::uint64_t segmentOrdinal) const;
    [[nodiscard]] ProtocolStatus MarkSegmentCompleted(
        std::uint64_t segmentOrdinal);
    [[nodiscard]] ProtocolResult<FinalManifest> PrepareFinalization();
    [[nodiscard]] ProtocolStatus LatchTerminalError(
        ProtocolErrorCode errorCode,
        std::size_t errorOffset) noexcept;

    SessionDescriptor sessionDescriptor_;
    ReceiverResourcePolicy resourcePolicy_;
    std::size_t segmentCount_ = 0;
    // Storage owns its PMR resource and both maps as one heap object. Moving the
    // state transfers this pointer without leaving a moved-from map that still
    // references a resource owned by the destination.
    std::unique_ptr<detail::DescriptorBindingStorage> descriptorStorage_;
    std::optional<FinalManifest> finalManifest_;
    ProtocolError terminalError_{};
};

} // namespace pbprotocol
