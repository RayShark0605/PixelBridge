#pragma once

#include "pbmodulation/unified_visual.h"
#include "pbreceiver/receiver_ingress.h"
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

namespace pbapp
{

// Output-only metadata. No payload, dynamic slot collection or decision input.
enum class ReceiverFrameReason : std::uint8_t
{
    NotEvaluated, Processed, BootstrapRejected, SessionMismatch, InvalidIdentity,
    Reordered, TelemetryOnly, FrameErased, OlderDuplicate, LargeOutputRejected,
    AlreadyPublished, OtherProfile, ProcessingFailed
};

enum class ReceiverSlotReason : std::uint8_t
{
    NotAttempted, AlreadyAdmitted, AlreadyPublished, WaitingForSession,
    WaitingForConfirmation, LargeOutputRejected, ControlAccepted, ControlUnknownSession,
    ControlRejected, TransportReturned, TransportUnknownSession, TransportResourceRejected,
    TransportRejected, SessionMismatch, ProcessingFailed
};

struct ReceiverDecisionState
{
    bool sessionReady = false;
    bool pendingConfirmation = false;
    bool receiverSessionBound = false;
    bool published = false;
};

struct ReceiverSlotDecision
{
    bool present = false;
    bool observedThisCall = false;
    pbmodulation::UnifiedSlotKind kind = pbmodulation::UnifiedSlotKind::Transport;
    ReceiverSlotReason reason = ReceiverSlotReason::NotAttempted;
    std::optional<pbprotocol::ControlRecordType> controlType;
    std::optional<std::uint64_t> segmentOrdinal;
    std::optional<std::uint32_t> outerBlockId;
    bool receiverCalled = false;
    bool receiverReturned = false;
    std::optional<pbreceiver::ReceiverDataDisposition> dataDisposition;
    std::optional<pbreceiver::ReceiverOuterSymbolAdmission> outerSymbolAdmission;
    std::optional<std::uint32_t> protocolError;
    bool resourceRejected = false;
    bool conflictRejected = false;
};

struct ReceiverDecisionTrace
{
    ReceiverFrameReason reason = ReceiverFrameReason::NotEvaluated;
    ReceiverDecisionState before;
    ReceiverDecisionState after;
    std::array<ReceiverSlotDecision, pbmodulation::kUnifiedCodewordCount> slots{};
};

inline void SetReceiverFrameReason(ReceiverDecisionTrace* const trace, const ReceiverFrameReason reason) noexcept
{
    if (trace)
    {
        trace->reason = reason;
    }
}

inline void SetReceiverSlotReason(ReceiverSlotDecision* const slot, const ReceiverSlotReason reason) noexcept
{
    if (slot)
    {
        slot->reason = reason;
    }
}

[[nodiscard]] inline std::string_view GetReceiverFrameReasonName(const ReceiverFrameReason reason) noexcept
{
    constexpr std::array names{"NotEvaluated", "Processed", "BootstrapRejected", "SessionMismatch", "InvalidIdentity",
        "Reordered", "TelemetryOnly", "FrameErased", "OlderDuplicate", "LargeOutputRejected", "AlreadyPublished", "OtherProfile", "ProcessingFailed"};
    const auto index = static_cast<std::size_t>(reason);
    return index < names.size() ? names[index] : "InvalidDiagnosticReason";
}

[[nodiscard]] inline std::string_view GetReceiverSlotReasonName(const ReceiverSlotReason reason) noexcept
{
    constexpr std::array names{"NotAttempted", "AlreadyAdmitted", "AlreadyPublished", "WaitingForSession",
        "WaitingForConfirmation", "LargeOutputRejected", "ControlAccepted", "ControlUnknownSession", "ControlRejected",
        "TransportReturned", "TransportUnknownSession", "TransportResourceRejected", "TransportRejected", "SessionMismatch", "ProcessingFailed"};
    const auto index = static_cast<std::size_t>(reason);
    return index < names.size() ? names[index] : "InvalidDiagnosticReason";
}

// Same hard limits as the existing offline trace, including checked subtraction.
[[nodiscard]] inline bool CanAppendRecordedTrace(const std::uint64_t writtenBytes, const std::uint64_t recordBytes) noexcept
{
    constexpr std::uint64_t maximumTraceBytes = 64ULL * 1024 * 1024;
    return recordBytes <= 65536 && writtenBytes <= maximumTraceBytes && recordBytes <= maximumTraceBytes - writtenBytes;
}

inline void WriteReceiverDecisionTrace(std::ostream& output, const ReceiverDecisionTrace& trace)
{
    const auto WriteState = [&output](const ReceiverDecisionState& state)
    {
        output << "{\"sessionReady\":" << (state.sessionReady ? "true" : "false")
            << ",\"pendingConfirmation\":" << (state.pendingConfirmation ? "true" : "false")
            << ",\"receiverSessionBound\":" << (state.receiverSessionBound ? "true" : "false")
            << ",\"published\":" << (state.published ? "true" : "false") << '}';
    };
    const auto WriteOptional = [&output](const auto& value)
    {
        if (value)
        {
            output << static_cast<std::uint64_t>(*value);
        }
        else
        {
            output << "null";
        }
    };
    output << "{\"schema\":\"PixelBridge.ReceiverDecisions.1\",\"frameReason\":\"" << GetReceiverFrameReasonName(trace.reason) << "\",\"before\":";
    WriteState(trace.before);
    output << ",\"after\":";
    WriteState(trace.after);
    output << ",\"slots\":[";
    bool first = true;
    for (std::size_t index = 0; index < trace.slots.size(); index++)
    {
        const auto& slot = trace.slots[index];
        if (!slot.present)
        {
            continue;
        }
        if (!first)
        {
            output << ',';
        }
        first = false;
        output << "{\"slot\":" << index << ",\"observedThisCall\":" << (slot.observedThisCall ? "true" : "false")
            << ",\"kind\":" << static_cast<unsigned>(slot.kind) << ",\"reason\":\"" << GetReceiverSlotReasonName(slot.reason)
            << "\",\"controlType\":";
        WriteOptional(slot.controlType);
        output << ",\"segmentOrdinal\":";
        WriteOptional(slot.segmentOrdinal);
        output << ",\"outerBlockId\":";
        WriteOptional(slot.outerBlockId);
        output << ",\"receiverCalled\":" << (slot.receiverCalled ? "true" : "false")
            << ",\"receiverReturned\":" << (slot.receiverReturned ? "true" : "false") << ",\"dataDisposition\":";
        WriteOptional(slot.dataDisposition);
        output << ",\"outerSymbolAdmission\":";
        WriteOptional(slot.outerSymbolAdmission);
        output << ",\"protocolError\":";
        WriteOptional(slot.protocolError);
        output << ",\"resourceRejected\":" << (slot.resourceRejected ? "true" : "false")
            << ",\"conflictRejected\":" << (slot.conflictRejected ? "true" : "false") << '}';
    }
    output << "]}";
}

} // namespace pbapp
