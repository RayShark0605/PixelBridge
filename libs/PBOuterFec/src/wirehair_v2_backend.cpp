#include "wirehair_v2_backend.h"

#include <wirehair/wirehair.h>

#include <cstdint>

namespace pbouterfec::detail {
namespace {

thread_local const WirehairV2Backend* backendOverride = nullptr;

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

} // namespace pbouterfec::detail
