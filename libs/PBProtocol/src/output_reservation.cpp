#include "pbprotocol/output_reservation.h"

#include "pbprotocol/descriptor_codec.h"

namespace pbprotocol {

ProtocolResult<OutputReservationDecision> EvaluateOutputReservation(
    const std::uint64_t originalFileSize,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    using ReservationResult = ProtocolResult<OutputReservationDecision>;

    // The policy gate runs before any decision so a misconfigured receiver
    // can never silently accept an oversized reservation.
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return ReservationResult::Failure(
            policyStatus.Error().code,
            policyStatus.Error().offset);
    }

    if (originalFileSize > resourcePolicy.maxAcceptedFileBytes)
    {
        return ReservationResult::Failure(
            ProtocolErrorCode::OutputReservationDenied,
            0);
    }
    if (originalFileSize >
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt)
    {
        return ReservationResult::Success(
            OutputReservationDecision::RequiresUserConfirmation);
    }
    return ReservationResult::Success(OutputReservationDecision::AutoAccept);
}

} // namespace pbprotocol
