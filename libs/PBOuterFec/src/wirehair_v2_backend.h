#pragma once

#include <cstdint>

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

} // namespace pbouterfec::detail
