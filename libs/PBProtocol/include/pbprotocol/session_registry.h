#pragma once

#include "pbprotocol/descriptor_binding.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace pbprotocol {

namespace test {

class SessionRegistryTestAccess;

} // namespace test

// SessionRegistry has one owning thread. It is the only hot-path routing seam:
// no SegmentDescriptor may reach a DescriptorBindingState until its SessionTag
// has a unique active SessionId binding. Ambiguous tags remain quarantined for
// the lifetime of the registry rather than being guessed or latest-wins.
class SessionRegistry
{
public:
    // Production admission and routing always use DeriveSessionTag().
    [[nodiscard]] static ProtocolResult<SessionRegistry> Create(
        ReceiverResourcePolicy resourcePolicy);

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;
    SessionRegistry(SessionRegistry&&) noexcept = default;
    SessionRegistry& operator=(SessionRegistry&&) noexcept = default;

    [[nodiscard]] ProtocolResult<DescriptorBindDisposition>
    BindSessionDescriptor(const SessionDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition>
    BindSegmentDescriptor(const SegmentDescriptor& descriptor);
    [[nodiscard]] ProtocolResult<DescriptorBindDisposition> BindFinalManifest(
        const FinalManifest& finalManifest);

    [[nodiscard]] ProtocolStatus ValidateCompleteSegmentMap(
        SessionTag sessionTag);

    // Success(false) means no Session existed. Inconsistent routing or budget
    // state is reported as InternalDescriptorStateError rather than absence.
    [[nodiscard]] ProtocolResult<bool> RemoveSession(
        const SessionId& sessionId) noexcept;
    [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;
    [[nodiscard]] std::uint64_t ReservedDescriptorStateBytes() const noexcept;
    [[nodiscard]] bool IsTagAmbiguous(SessionTag sessionTag) const noexcept;

private:
    using SessionTagDeriver = SessionTag (*)(const SessionId&) noexcept;

    friend class test::SessionRegistryTestAccess;

    struct SessionEntry
    {
        SessionTag sessionTag{};
        std::unique_ptr<DescriptorBindingState> bindingState;
    };

    struct TagBinding
    {
        SessionId sessionId{};
        bool ambiguous = false;
    };

    SessionRegistry(
        ReceiverResourcePolicy resourcePolicy,
        SessionTagDeriver sessionTagDeriver) noexcept;

    [[nodiscard]] static ProtocolResult<SessionRegistry>
    CreateWithSessionTagDeriverForTesting(
        ReceiverResourcePolicy resourcePolicy,
        SessionTagDeriver sessionTagDeriver);

    [[nodiscard]] ProtocolResult<DescriptorBindingState*> FindBindingState(
        SessionTag sessionTag) noexcept;
    [[nodiscard]] ProtocolStatus RemoveUniqueSessionForCollision(
        SessionTag sessionTag,
        TagBinding& tagBinding) noexcept;
    [[nodiscard]] bool HasConsistentRegistryState() const noexcept;

    ReceiverResourcePolicy resourcePolicy_;
    SessionTagDeriver sessionTagDeriver_ = nullptr;
    std::uint64_t reservedDescriptorStateBytes_ = 0;
    std::map<std::array<std::byte, kSessionIdBytes>, SessionEntry> sessionsById_;
    std::map<std::uint64_t, TagBinding> bindingsByTag_;
};

} // namespace pbprotocol
