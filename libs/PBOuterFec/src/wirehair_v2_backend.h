#pragma once

#include "pbouterfec/outer_fec_result.h"

#include <cstdint>
#include <optional>

namespace pbouterfec::detail {

inline constexpr int kWirehairV2Success = 0;
inline constexpr int kWirehairV2NeedMore = 1;
inline constexpr int kWirehairV2InvalidInput = 2;
inline constexpr int kWirehairV2BufferTooSmall = 3;
inline constexpr int kWirehairV2InvalidMagic = 4;
inline constexpr int kWirehairV2UnsupportedVersion = 5;
inline constexpr int kWirehairV2InvalidSize = 6;
inline constexpr int kWirehairV2ReservedNonzero = 7;
inline constexpr int kWirehairV2UnsupportedProfile = 8;
inline constexpr int kWirehairV2InvalidDimensions = 9;
inline constexpr int kWirehairV2BadSeed = 10;
inline constexpr int kWirehairV2ExtraInsufficient = 11;
inline constexpr int kWirehairV2Error = 12;
inline constexpr int kWirehairV2OutOfMemory = 13;
inline constexpr int kWirehairV2UnsupportedPlatform = 14;

struct WirehairV2ProfileFields
{
    std::uint64_t profileId = 0;
    std::uint64_t messageBytes = 0;
    std::uint32_t blockBytes = 0;
    std::uint8_t seedAttempt = 0;
};

struct WirehairV2Backend
{
    int (*profileValidate)(const void*, std::uint32_t) = nullptr;
    int (*profileDeserialize)(
        const void*, std::uint32_t, WirehairV2ProfileFields*) = nullptr;
    int (*encoderCreateProfileId)(
        std::uint64_t,
        const void*,
        std::uint64_t,
        std::uint32_t,
        void*,
        std::uint32_t,
        std::uint32_t*,
        void**) = nullptr;
    int (*encoderCreateProfile)(
        const void*, const void*, std::uint32_t, void**) = nullptr;
    int (*decoderCreate)(const void*, std::uint32_t, void**) = nullptr;
    int (*encode)(
        void*, std::uint32_t, void*, std::uint32_t, std::uint32_t*) = nullptr;
    int (*decode)(
        void*, std::uint32_t, const void*, std::uint32_t) = nullptr;
    int (*recover)(void*, void*, std::uint64_t, std::uint64_t*) = nullptr;
    void (*freeCodec)(void*) = nullptr;
};

[[nodiscard]] const WirehairV2Backend& GetWirehairV2Backend() noexcept;

// The wrapper's accepted-ID table is independent from Wirehair's private
// table. A per-decoder CSPRNG salt prevents a sender from precomputing the
// wrapper's bounded linear-probe clusters. This is DoS hardening only.
[[nodiscard]] std::uint64_t HashWirehairV2AcceptedBlockId(
    std::uint32_t outerBlockId,
    std::uint64_t hashSalt) noexcept;

[[nodiscard]] OuterFecResult<std::uint64_t>
GenerateWirehairV2AcceptedBlockHashSalt() noexcept;

// Private deterministic fault-injection seam. It is deliberately absent from
// installed/public headers. Overrides are thread-local and nest safely, so
// tests do not alter another thread's codec backend.
class ScopedWirehairV2BackendOverride
{
public:
    explicit ScopedWirehairV2BackendOverride(
        const WirehairV2Backend& backend) noexcept;
    ScopedWirehairV2BackendOverride(
        const ScopedWirehairV2BackendOverride&) = delete;
    ScopedWirehairV2BackendOverride& operator=(
        const ScopedWirehairV2BackendOverride&) = delete;
    ~ScopedWirehairV2BackendOverride();

private:
    const WirehairV2Backend* previousBackend_ = nullptr;
};

// Private deterministic seam for collision tests. Production decoders always
// obtain a fresh salt from the OS CSPRNG.
class ScopedWirehairV2AcceptedBlockHashSaltOverride
{
public:
    explicit ScopedWirehairV2AcceptedBlockHashSaltOverride(
        std::uint64_t hashSalt) noexcept;
    ScopedWirehairV2AcceptedBlockHashSaltOverride(
        const ScopedWirehairV2AcceptedBlockHashSaltOverride&) = delete;
    ScopedWirehairV2AcceptedBlockHashSaltOverride& operator=(
        const ScopedWirehairV2AcceptedBlockHashSaltOverride&) = delete;
    ~ScopedWirehairV2AcceptedBlockHashSaltOverride();

private:
    std::optional<std::uint64_t> previousHashSalt_;
};

} // namespace pbouterfec::detail
