#pragma once

#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/descriptor_binding.h"
#include "pbprotocol/session_registry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

namespace pbprotocol {

struct ControlRecordAdmission
{
    ControlRecordType recordType = static_cast<ControlRecordType>(0);
    std::uint64_t controlSequence = 0;
    SessionTag sessionTag{};
    DescriptorBindDisposition bindDisposition =
        DescriptorBindDisposition::Inserted;

    bool operator==(const ControlRecordAdmission&) const = default;
};

enum class ControlFragmentReceiveDisposition : std::uint8_t
{
    Stored,
    Repeated,
    DescriptorInserted,
    DescriptorRepeated
};

struct ControlFragmentReceiveResult
{
    ControlFragmentReceiveDisposition disposition =
        ControlFragmentReceiveDisposition::Stored;
    std::optional<ControlRecordAdmission> admission;

    bool operator==(const ControlFragmentReceiveResult&) const = default;
};

namespace detail {

struct ControlPlaneReceiverImplementation;
struct ControlReassemblyRecord;

} // namespace detail

// ControlPlaneReceiver has one owning thread. It is the authoritative ingress
// boundary for complete and fragmented PB-Control-1 records; callers must not
// treat successful envelope parsing alone as descriptor admission.
class ControlPlaneReceiver
{
public:
    [[nodiscard]] static ProtocolResult<ControlPlaneReceiver> Create(
        ReceiverResourcePolicy resourcePolicy);

    // The bounded reassembly resource remains authoritative. This overload is
    // the allocator fault-injection seam; the receiver retains shared ownership
    // and destroys all referring containers before releasing the resource.
    [[nodiscard]] static ProtocolResult<ControlPlaneReceiver>
    CreateWithMemoryResource(
        ReceiverResourcePolicy resourcePolicy,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource);

    ControlPlaneReceiver(const ControlPlaneReceiver&) = delete;
    ControlPlaneReceiver& operator=(const ControlPlaneReceiver&) = delete;
    ~ControlPlaneReceiver();
    ControlPlaneReceiver(ControlPlaneReceiver&&) noexcept;
    ControlPlaneReceiver& operator=(ControlPlaneReceiver&&) noexcept;

    [[nodiscard]] ProtocolResult<ControlRecordAdmission> ReceiveControlRecord(
        std::span<const std::byte> recordBytes);
    [[nodiscard]] ProtocolResult<ControlFragmentReceiveResult>
    ReceiveControlFragment(
        std::span<const std::byte> fragmentBytes,
        std::uint64_t observationOrdinal);

    [[nodiscard]] ProtocolStatus AdvanceObservationOrdinal(
        std::uint64_t observationOrdinal) noexcept;
    void ResetControlReassembly() noexcept;

    [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;
    [[nodiscard]] std::uint64_t ReservedDescriptorStateBytes() const noexcept;
    [[nodiscard]] std::size_t ActiveControlReassemblyCount() const noexcept;
    // Includes allocator/container bookkeeping charged by the active standard
    // library implementation. A newly created or reset receiver has no engaged
    // reassembly container and therefore reports zero.
    [[nodiscard]] std::size_t ControlReassemblyBytesInUse() const noexcept;
    // Cumulative count of receive operations rejected by resource-policy
    // gates (ResourceLimitExceeded / ResourceExhausted /
    // ControlReassemblyQuotaExceeded) on both the complete-record and
    // fragment paths. Survives ResetControlReassembly(); telemetry only,
    // never serialized into wire bytes.
    [[nodiscard]] std::uint64_t GetRejectedByResourcePolicyCount() const noexcept;
    [[nodiscard]] bool IsTagAmbiguous(SessionTag sessionTag) const noexcept;
    [[nodiscard]] ProtocolResult<std::size_t> BoundSegmentCount(
        SessionTag sessionTag) const noexcept;
    [[nodiscard]] ProtocolResult<bool> HasFinalManifest(
        SessionTag sessionTag) const noexcept;
    [[nodiscard]] ProtocolStatus ValidateCompleteSegmentMap(
        SessionTag sessionTag);
    [[nodiscard]] ProtocolResult<bool> RemoveSession(
        const SessionId& sessionId) noexcept;

private:
    explicit ControlPlaneReceiver(
        std::unique_ptr<detail::ControlPlaneReceiverImplementation>
            implementation) noexcept;

    [[nodiscard]] ProtocolResult<ControlRecordAdmission> ParseValidateAndBind(
        std::span<const std::byte> recordBytes);

    // Body of the public fragment entry point, split out so that wrapper can
    // count resource-policy rejections exactly once per receive operation.
    [[nodiscard]] ProtocolResult<ControlFragmentReceiveResult>
    ReceiveControlFragmentCore(
        std::span<const std::byte> fragmentBytes,
        std::uint64_t observationOrdinal);

    // Reassembles a complete record and admits it through the non-counting
    // ParseValidateAndBind path; telemetry is applied by the public wrapper.
    [[nodiscard]] static ProtocolResult<ControlFragmentReceiveResult>
    AttemptAdmission(
        ControlPlaneReceiver& receiver,
        detail::ControlPlaneReceiverImplementation& implementation,
        detail::ControlReassemblyRecord& record);

    std::unique_ptr<detail::ControlPlaneReceiverImplementation> implementation_;
};

} // namespace pbprotocol
