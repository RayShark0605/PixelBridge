#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>

#else

#error "GenerateRandomSessionId() is only implemented for Windows."

#endif

namespace pbprotocol::detail {

// Internal instance seam around the exact BCryptGenRandom ABI. Production
// supplies the real Windows implementation; tests use stack-owned recorders so
// no mutable process-global RNG hook can affect concurrent callers.
class BCryptRandomApi
{
public:
    virtual ~BCryptRandomApi() = default;

    [[nodiscard]] virtual NTSTATUS GenerateRandom(
        BCRYPT_ALG_HANDLE algorithmHandle,
        PUCHAR buffer,
        ULONG bufferByteCount,
        ULONG flags) noexcept = 0;
};

[[nodiscard]] ProtocolResult<SessionId> GenerateRandomSessionIdWithApi(
    BCryptRandomApi& bcryptRandomApi) noexcept;

} // namespace pbprotocol::detail
