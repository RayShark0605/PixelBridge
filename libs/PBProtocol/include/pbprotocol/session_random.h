#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <span>

namespace pbprotocol {

// Generates a fresh 128-bit SessionId from the OS CSPRNG (design doc 7.4):
// BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG on Windows. Timestamps,
// process IDs, rand() and std::random_device are forbidden for SessionIds.
[[nodiscard]] ProtocolResult<SessionId> GenerateRandomSessionId() noexcept;

namespace test {

class SessionRandomTestAccess;

} // namespace test

namespace detail {

// Signature of an injectable byte source. Production is hard-wired to the OS
// CSPRNG (DrawSystemBytes); tests may substitute a deterministic or failing
// source through SessionRandomTestAccess without any global state.
using SystemRngFunction = ProtocolStatus (*)(std::span<std::byte>) noexcept;

// The single place that touches the Windows BCrypt CSPRNG. Generate() is the
// only production entry point and is hard-wired to DrawSystemBytes(); the
// injectable seam stays private so no production code can substitute a source.
class SystemCsprngSource
{
public:
    [[nodiscard]] static ProtocolResult<SessionId> Generate() noexcept;

private:
    friend class test::SessionRandomTestAccess;

    // Fills buffer from BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG).
    // Any NTSTATUS failure is fail-closed as CsprngFailure.
    [[nodiscard]] static ProtocolStatus DrawSystemBytes(
        std::span<std::byte> buffer) noexcept;

    // Test seam only: draws kSessionIdBytes through the given source and
    // packages them into a SessionId. A null source is an internal invariant
    // violation, never a success.
    [[nodiscard]] static ProtocolResult<SessionId> GenerateWithRng(
        SystemRngFunction rngFunction) noexcept;
};

} // namespace detail

} // namespace pbprotocol