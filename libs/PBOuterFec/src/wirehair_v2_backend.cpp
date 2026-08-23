#include "wirehair_v2_backend.h"

#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/session_random.h"

#include <wirehair/wirehair.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace pbouterfec::detail {
namespace {

static_assert(kWirehairV2Success == static_cast<int>(WirehairV2_Success));
static_assert(kWirehairV2NeedMore == static_cast<int>(WirehairV2_NeedMore));
static_assert(kWirehairV2InvalidInput ==
    static_cast<int>(WirehairV2_InvalidInput));
static_assert(kWirehairV2BufferTooSmall ==
    static_cast<int>(WirehairV2_BufferTooSmall));
static_assert(kWirehairV2InvalidMagic ==
    static_cast<int>(WirehairV2_InvalidMagic));
static_assert(kWirehairV2UnsupportedVersion ==
    static_cast<int>(WirehairV2_UnsupportedVersion));
static_assert(kWirehairV2InvalidSize ==
    static_cast<int>(WirehairV2_InvalidSize));
static_assert(kWirehairV2ReservedNonzero ==
    static_cast<int>(WirehairV2_ReservedNonzero));
static_assert(kWirehairV2UnsupportedProfile ==
    static_cast<int>(WirehairV2_UnsupportedProfile));
static_assert(kWirehairV2InvalidDimensions ==
    static_cast<int>(WirehairV2_InvalidDimensions));
static_assert(kWirehairV2BadSeed == static_cast<int>(WirehairV2_BadSeed));
static_assert(kWirehairV2ExtraInsufficient ==
    static_cast<int>(WirehairV2_ExtraInsufficient));
static_assert(kWirehairV2Error == static_cast<int>(WirehairV2_Error));
static_assert(kWirehairV2OutOfMemory == static_cast<int>(WirehairV2_OOM));
static_assert(kWirehairV2UnsupportedPlatform ==
    static_cast<int>(WirehairV2_UnsupportedPlatform));
static_assert(kWirehairV2CertifiedProfileId ==
    WIREHAIR_V2_PROFILE_CERTIFIED_2026_07);
static_assert(kWirehairV2MixedProfileId ==
    WIREHAIR_V2_PROFILE_MIXED_2026_07);
static_assert(kWirehairV2MixedMix2ProfileId ==
    WIREHAIR_V2_PROFILE_MIXED_MIX2_2026_07);
static_assert(pbprotocol::kWirehairV2SerializedProfileBytes ==
    WIREHAIR_V2_PROFILE_SERIALIZED_BYTES);
static_assert(sizeof(WirehairV2Profile) ==
    WIREHAIR_V2_PROFILE_SERIALIZED_BYTES);

thread_local const WirehairV2Backend* backendOverride = nullptr;
thread_local std::optional<std::uint64_t> acceptedBlockHashSaltOverride;

[[nodiscard]] std::uint64_t MixAcceptedBlockHash(
    std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31U;
    return value;
}

[[nodiscard]] int ProfileValidate(
    const void* const serializedProfile,
    const std::uint32_t serializedBytes)
{
    return static_cast<int>(wirehair_v2_profile_validate(
        serializedProfile, serializedBytes));
}

[[nodiscard]] int ProfileDeserialize(
    const void* const serializedProfile,
    const std::uint32_t serializedBytes,
    WirehairV2ProfileFields* const profileFields)
{
    if (profileFields == nullptr)
    {
        return kWirehairV2InvalidInput;
    }

    WirehairV2Profile profile{};
    const WirehairV2Result result = wirehair_v2_profile_deserialize(
        serializedProfile, serializedBytes, &profile);
    if (result != WirehairV2_Success)
    {
        return static_cast<int>(result);
    }

    profileFields->profileId = profile.profile_id;
    profileFields->messageBytes = profile.message_bytes;
    profileFields->blockBytes = profile.block_bytes;
    profileFields->seedAttempt = profile.seed_attempt;
    return kWirehairV2Success;
}

[[nodiscard]] int EncoderCreateProfileId(
    const std::uint64_t profileId,
    const void* const message,
    const std::uint64_t messageBytes,
    const std::uint32_t blockBytes,
    void* const serializedProfileOut,
    const std::uint32_t serializedProfileCapacity,
    std::uint32_t* const serializedProfileBytesOut,
    void** const codecOut)
{
    if (codecOut == nullptr)
    {
        return kWirehairV2InvalidInput;
    }

    WirehairV2Codec codec = nullptr;
    const WirehairV2Result result = wirehair_v2_encoder_create_profile_id(
        profileId,
        message,
        messageBytes,
        blockBytes,
        serializedProfileOut,
        serializedProfileCapacity,
        serializedProfileBytesOut,
        &codec);
    *codecOut = static_cast<void*>(codec);
    return static_cast<int>(result);
}

[[nodiscard]] int EncoderCreateProfile(
    const void* const message,
    const void* const serializedProfile,
    const std::uint32_t serializedProfileBytes,
    void** const codecOut)
{
    if (codecOut == nullptr)
    {
        return kWirehairV2InvalidInput;
    }

    WirehairV2Codec codec = nullptr;
    const WirehairV2Result result = wirehair_v2_encoder_create_profile(
        message, serializedProfile, serializedProfileBytes, &codec);
    *codecOut = static_cast<void*>(codec);
    return static_cast<int>(result);
}

[[nodiscard]] int DecoderCreate(
    const void* const serializedProfile,
    const std::uint32_t serializedProfileBytes,
    void** const codecOut)
{
    if (codecOut == nullptr)
    {
        return kWirehairV2InvalidInput;
    }

    WirehairV2Codec codec = nullptr;
    const WirehairV2Result result = wirehair_v2_decoder_create(
        serializedProfile, serializedProfileBytes, &codec);
    *codecOut = static_cast<void*>(codec);
    return static_cast<int>(result);
}

[[nodiscard]] int Encode(
    void* const codec,
    const std::uint32_t blockId,
    void* const blockDataOut,
    const std::uint32_t outputCapacity,
    std::uint32_t* const dataBytesOut)
{
    return static_cast<int>(wirehair_v2_encode(
        static_cast<WirehairV2Codec>(codec),
        blockId,
        blockDataOut,
        outputCapacity,
        dataBytesOut));
}

[[nodiscard]] int Decode(
    void* const codec,
    const std::uint32_t blockId,
    const void* const blockData,
    const std::uint32_t dataBytes)
{
    return static_cast<int>(wirehair_v2_decode(
        static_cast<WirehairV2Codec>(codec),
        blockId,
        blockData,
        dataBytes));
}

[[nodiscard]] int Recover(
    void* const codec,
    void* const messageOut,
    const std::uint64_t outputCapacity,
    std::uint64_t* const bytesOut)
{
    return static_cast<int>(wirehair_v2_recover(
        static_cast<WirehairV2Codec>(codec),
        messageOut,
        outputCapacity,
        bytesOut));
}

void FreeCodec(void* const codec)
{
    wirehair_v2_free(static_cast<WirehairV2Codec>(codec));
}

const WirehairV2Backend defaultBackend{
    &ProfileValidate,
    &ProfileDeserialize,
    &EncoderCreateProfileId,
    &EncoderCreateProfile,
    &DecoderCreate,
    &Encode,
    &Decode,
    &Recover,
    &FreeCodec};

} // namespace

const WirehairV2Backend& GetWirehairV2Backend() noexcept
{
    return backendOverride == nullptr ? defaultBackend : *backendOverride;
}

std::uint64_t HashWirehairV2AcceptedBlockId(
    const std::uint32_t outerBlockId,
    const std::uint64_t hashSalt) noexcept
{
    return MixAcceptedBlockHash(
        static_cast<std::uint64_t>(outerBlockId) ^ hashSalt);
}

OuterFecResult<std::uint64_t>
GenerateWirehairV2AcceptedBlockHashSalt() noexcept
{
    if (acceptedBlockHashSaltOverride.has_value())
    {
        return OuterFecResult<std::uint64_t>::Success(
            acceptedBlockHashSaltOverride.value());
    }

    const auto randomResult = pbprotocol::GenerateRandomSessionId();
    if (!randomResult)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::CsprngFailure,
            static_cast<std::uint64_t>(randomResult.Error().code));
    }

    std::uint64_t firstHalf = 0;
    std::uint64_t secondHalf = 0;
    for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        const unsigned int shift = static_cast<unsigned int>(byteIndex * 8U);
        firstHalf |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(
                randomResult.Value().bytes[byteIndex])) << shift;
        secondHalf |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(
                randomResult.Value().bytes[byteIndex + 8U])) << shift;
    }
    return OuterFecResult<std::uint64_t>::Success(
        MixAcceptedBlockHash(firstHalf) ^
        MixAcceptedBlockHash(secondHalf ^ UINT64_C(0x9E3779B97F4A7C15)));
}

ScopedWirehairV2BackendOverride::ScopedWirehairV2BackendOverride(
    const WirehairV2Backend& backend) noexcept
    : previousBackend_(backendOverride)
{
    backendOverride = &backend;
}

ScopedWirehairV2BackendOverride::~ScopedWirehairV2BackendOverride()
{
    backendOverride = previousBackend_;
}

ScopedWirehairV2AcceptedBlockHashSaltOverride::
ScopedWirehairV2AcceptedBlockHashSaltOverride(
    const std::uint64_t hashSalt) noexcept
    : previousHashSalt_(acceptedBlockHashSaltOverride)
{
    acceptedBlockHashSaltOverride = hashSalt;
}

ScopedWirehairV2AcceptedBlockHashSaltOverride::
~ScopedWirehairV2AcceptedBlockHashSaltOverride()
{
    acceptedBlockHashSaltOverride = previousHashSalt_;
}

} // namespace pbouterfec::detail
