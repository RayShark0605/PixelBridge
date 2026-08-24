#include "pbouterfec/outer_fec_decoder_resource.h"

#include "outer_fec_decoder_resource_internal.h"
#include "pbprotocol/descriptor_codec.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace pbouterfec::detail
{

OuterFecDecoderReservation::OuterFecDecoderReservation(
    std::shared_ptr<OuterFecDecoderResourceState> state,
    const std::uint64_t reservationBytes) noexcept
    : state_(std::move(state))
    , reservationBytes_(reservationBytes)
{
}

OuterFecDecoderReservation::OuterFecDecoderReservation(
    OuterFecDecoderReservation&& other) noexcept
    : state_(std::move(other.state_))
    , reservationBytes_(other.reservationBytes_)
{
    other.reservationBytes_ = 0;
}

OuterFecDecoderReservation& OuterFecDecoderReservation::operator=(
    OuterFecDecoderReservation&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Release();
    state_ = std::move(other.state_);
    reservationBytes_ = other.reservationBytes_;
    other.reservationBytes_ = 0;
    return *this;
}

OuterFecDecoderReservation::~OuterFecDecoderReservation()
{
    Release();
}

void OuterFecDecoderReservation::Release() noexcept
{
    if (state_ == nullptr)
    {
        return;
    }

    const std::shared_ptr<OuterFecDecoderResourceState> state =
        std::move(state_);
    {
        const std::scoped_lock lock(state->mutex);
        if (state->activeDecoderCount > 0 &&
            state->reservedDecoderBytes >= reservationBytes_)
        {
            state->activeDecoderCount--;
            state->reservedDecoderBytes -= reservationBytes_;
        }
    }
    reservationBytes_ = 0;
}

void CountOuterFecDecoderQuotaExceeded(
    const std::shared_ptr<OuterFecDecoderResourceState>& state) noexcept
{
    if (state != nullptr)
    {
        state->quotaExceededCount.fetch_add(1ULL, std::memory_order_relaxed);
    }
}

OuterFecResult<OuterFecDecoderReservation>
AcquireOuterFecDecoderReservation(
    const std::shared_ptr<OuterFecDecoderResourceState>& state,
    const std::uint64_t reservationBytes)
{
    if (state == nullptr)
    {
        return OuterFecResult<OuterFecDecoderReservation>::Failure(
            OuterFecErrorCode::InvalidState);
    }

    if (reservationBytes > state->resourcePolicy.maxOuterFecDecoderBytes)
    {
        CountOuterFecDecoderQuotaExceeded(state);
        return OuterFecResult<OuterFecDecoderReservation>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            reservationBytes);
    }

    const std::scoped_lock lock(state->mutex);
    if (state->activeDecoderCount >=
        state->resourcePolicy.maxActiveOuterFecDecoders)
    {
        CountOuterFecDecoderQuotaExceeded(state);
        return OuterFecResult<OuterFecDecoderReservation>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            state->activeDecoderCount + 1ULL);
    }
    if (state->reservedDecoderBytes >
            state->resourcePolicy.maxTotalOuterFecDecoderBytes ||
        reservationBytes >
            state->resourcePolicy.maxTotalOuterFecDecoderBytes -
                state->reservedDecoderBytes)
    {
        CountOuterFecDecoderQuotaExceeded(state);
        return OuterFecResult<OuterFecDecoderReservation>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            reservationBytes);
    }

    state->activeDecoderCount++;
    state->reservedDecoderBytes += reservationBytes;
    return OuterFecResult<OuterFecDecoderReservation>::Success(
        OuterFecDecoderReservation(state, reservationBytes));
}

} // namespace pbouterfec::detail

namespace pbouterfec
{

OuterFecDecoderResourceManager::OuterFecDecoderResourceManager(
    OuterFecDecoderResourceManager&& other) noexcept = default;

OuterFecDecoderResourceManager&
OuterFecDecoderResourceManager::operator=(
    OuterFecDecoderResourceManager&& other) noexcept = default;

OuterFecDecoderResourceManager::~OuterFecDecoderResourceManager() = default;

OuterFecResult<OuterFecDecoderResourceManager>
OuterFecDecoderResourceManager::Create(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const pbprotocol::ProtocolStatus policyStatus =
        pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy);
    if (!policyStatus)
    {
        return OuterFecResult<OuterFecDecoderResourceManager>::Failure(
            OuterFecErrorCode::InvalidResourcePolicy,
            policyStatus.Error().offset);
    }

    try
    {
        OuterFecDecoderResourceManager resourceManager;
        resourceManager.state_ =
            std::make_shared<detail::OuterFecDecoderResourceState>(
                resourcePolicy);
        return OuterFecResult<OuterFecDecoderResourceManager>::Success(
            std::move(resourceManager));
    }
    catch (const std::bad_alloc&)
    {
        return OuterFecResult<OuterFecDecoderResourceManager>::Failure(
            OuterFecErrorCode::OutOfMemory);
    }
}

std::uint64_t OuterFecDecoderResourceManager::GetActiveDecoderCount() const
{
    if (state_ == nullptr)
    {
        return 0;
    }
    const std::scoped_lock lock(state_->mutex);
    return state_->activeDecoderCount;
}

std::uint64_t OuterFecDecoderResourceManager::GetReservedDecoderBytes() const
{
    if (state_ == nullptr)
    {
        return 0;
    }
    const std::scoped_lock lock(state_->mutex);
    return state_->reservedDecoderBytes;
}

std::uint64_t OuterFecDecoderResourceManager::GetQuotaExceededCount() const noexcept
{
    if (state_ == nullptr)
    {
        return 0;
    }
    // Relaxed load without the mutex: this is telemetry, not admission state.
    return state_->quotaExceededCount.load(std::memory_order_relaxed);
}

} // namespace pbouterfec
