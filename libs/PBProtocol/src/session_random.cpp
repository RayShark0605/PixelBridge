#include "pbprotocol/session_random.h"
#include "session_random_internal.h"

#include <limits>
#include <utility>

namespace pbprotocol {

namespace detail {

namespace {

class WindowsBCryptRandomApi final : public BCryptRandomApi
{
public:
    [[nodiscard]] NTSTATUS GenerateRandom(
        const BCRYPT_ALG_HANDLE algorithmHandle,
        PUCHAR const buffer,
        const ULONG bufferByteCount,
        const ULONG flags) noexcept override
    {
        return BCryptGenRandom(
            algorithmHandle,
            buffer,
            bufferByteCount,
            flags);
    }
};

} // namespace

ProtocolResult<SessionId> GenerateRandomSessionIdWithApi(
    BCryptRandomApi& bcryptRandomApi) noexcept
{
    static_assert(kSessionIdBytes <= std::numeric_limits<ULONG>::max());
    constexpr ULONG sessionIdByteCount = static_cast<ULONG>(kSessionIdBytes);

    SessionId sessionId{};
    const NTSTATUS status = bcryptRandomApi.GenerateRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(sessionId.bytes.data()),
        sessionIdByteCount,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status))
    {
        return ProtocolResult<SessionId>::Failure(
            ProtocolErrorCode::CsprngFailure,
            0);
    }

    return ProtocolResult<SessionId>::Success(std::move(sessionId));
}

} // namespace detail

ProtocolResult<SessionId> GenerateRandomSessionId() noexcept
{
    detail::WindowsBCryptRandomApi bcryptRandomApi;
    return detail::GenerateRandomSessionIdWithApi(bcryptRandomApi);
}

} // namespace pbprotocol
