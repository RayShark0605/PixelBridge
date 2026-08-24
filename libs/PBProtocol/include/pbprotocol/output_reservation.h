#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstdint>

namespace pbprotocol {

// Policy-level decision for creating or extending a .part file before any
// disk preallocation happens. This API is stateless, so the returned status
// IS the observable event: application layers record AutoAccept /
// RequiresUserConfirmation / OutputReservationDenied as their telemetry. Disk
// free-space checks belong to the storage layer (design section 33.3) and run
// after this gate passes.
enum class OutputReservationDecision : std::uint8_t
{
    // Within maxOutputPreallocationBytesWithoutPrompt: proceed without asking.
    AutoAccept,
    // Above the prompt threshold but within maxAcceptedFileBytes: require an
    // explicit user confirmation before preallocating.
    RequiresUserConfirmation
};

// originalFileSize is the sender-declared size from the session descriptor.
// Sizes above maxAcceptedFileBytes fail with OutputReservationDenied; sizes
// above the prompt threshold succeed with RequiresUserConfirmation; everything
// else succeeds with AutoAccept. An invalid policy fails closed with
// InvalidResourcePolicy before any decision is made.
[[nodiscard]] ProtocolResult<OutputReservationDecision> EvaluateOutputReservation(
    std::uint64_t originalFileSize,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

} // namespace pbprotocol
