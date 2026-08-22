#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

namespace pbprotocol {

// Generates a fresh 128-bit SessionId from the OS CSPRNG (design doc 7.4):
// BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG on Windows. Timestamps,
// process IDs, rand() and std::random_device are forbidden for SessionIds.
[[nodiscard]] ProtocolResult<SessionId> GenerateRandomSessionId() noexcept;

} // namespace pbprotocol
