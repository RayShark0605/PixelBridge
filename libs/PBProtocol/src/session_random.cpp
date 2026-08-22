#include "pbprotocol/session_random.h"

#if defined(_WIN32)

// windows.h defines min/max macros that collide with std::numeric_limits.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>

#else

#error "GenerateRandomSessionId() is only implemented for Windows."

#endif

#include <limits>

namespace pbprotocol {

ProtocolResult<SessionId> GenerateRandomSessionId() noexcept
{
    return detail::SystemCsprngSource::Generate();
}

namespace detail {

ProtocolResult<SessionId> SystemCsprngSource::Generate() noexcept
{
    return GenerateWithRng(&DrawSystemBytes);
}

ProtocolStatus SystemCsprngSource::DrawSystemBytes(
    std::span<std::byte> buffer) noexcept
{
    if (buffer.empty())
    {
        return ProtocolStatus::Success();
    }

    // BCryptGenRandom takes a ULONG byte count; refuse to truncate instead of
    // silently drawing fewer bytes than the caller expects.
    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::CsprngFailure, 0);
    }

    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(buffer.data()),
        static_cast<ULONG>(buffer.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::CsprngFailure, 0);
    }

    return ProtocolStatus::Success();
}

ProtocolResult<SessionId> SystemCsprngSource::GenerateWithRng(
    const SystemRngFunction rngFunction) noexcept
{
    if (rngFunction == nullptr)
    {
        return ProtocolResult<SessionId>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }

    SessionId sessionId{};
    const ProtocolStatus drawStatus = rngFunction(sessionId.bytes);
    if (!drawStatus)
    {
        return ProtocolResult<SessionId>::Failure(
            drawStatus.Error().code,
            drawStatus.Error().offset);
    }

    return ProtocolResult<SessionId>::Success(std::move(sessionId));
}

} // namespace detail

} // namespace pbprotocol