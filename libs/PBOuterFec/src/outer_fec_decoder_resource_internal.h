#pragma once

#include "pbouterfec/outer_fec_decoder_resource.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace pbouterfec::detail
{

struct OuterFecDecoderResourceState
{
    explicit OuterFecDecoderResourceState(
        const pbprotocol::ReceiverResourcePolicy& receiverResourcePolicy)
        : resourcePolicy(receiverResourcePolicy)
    {
    }

    pbprotocol::ReceiverResourcePolicy resourcePolicy{};
    mutable std::mutex mutex;
    std::uint64_t activeDecoderCount = 0;
    std::uint64_t reservedDecoderBytes = 0;
};

class OuterFecDecoderReservation
{
public:
    OuterFecDecoderReservation() noexcept = default;
    OuterFecDecoderReservation(
        std::shared_ptr<OuterFecDecoderResourceState> state,
        std::uint64_t reservationBytes) noexcept;

    OuterFecDecoderReservation(const OuterFecDecoderReservation&) = delete;
    OuterFecDecoderReservation& operator=(
        const OuterFecDecoderReservation&) = delete;
    OuterFecDecoderReservation(
        OuterFecDecoderReservation&& other) noexcept;
    OuterFecDecoderReservation& operator=(
        OuterFecDecoderReservation&& other) noexcept;
    ~OuterFecDecoderReservation();

private:
    void Release() noexcept;

    std::shared_ptr<OuterFecDecoderResourceState> state_;
    std::uint64_t reservationBytes_ = 0;
};

[[nodiscard]] OuterFecResult<OuterFecDecoderReservation>
AcquireOuterFecDecoderReservation(
    const std::shared_ptr<OuterFecDecoderResourceState>& state,
    std::uint64_t reservationBytes);

} // namespace pbouterfec::detail
