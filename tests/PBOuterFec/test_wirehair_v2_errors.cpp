#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "wirehair_v2_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace {

using pbouterfec::detail::WirehairV2Backend;
using pbouterfec::detail::WirehairV2ProfileFields;

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

const std::array<std::byte, 32> kCanonicalProfile{
    Byte(0x57), Byte(0x48), Byte(0x56), Byte(0x32),
    Byte(0x01), Byte(0x00), Byte(0x20), Byte(0x00),
    Byte(0xC9), Byte(0xF9), Byte(0xF4), Byte(0x47),
    Byte(0xBB), Byte(0x5B), Byte(0x29), Byte(0x4B),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x10), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00)};

struct FakeBackendState
{
    int profileValidateResult = pbouterfec::detail::kWirehairV2Success;
    int profileDeserializeResult = pbouterfec::detail::kWirehairV2Success;
    int encoderCreateProfileIdResult =
        pbouterfec::detail::kWirehairV2Success;
    int encoderCreateProfileResult =
        pbouterfec::detail::kWirehairV2Success;
    int decoderCreateResult = pbouterfec::detail::kWirehairV2Success;
    int encodeResult = pbouterfec::detail::kWirehairV2Success;
    int decodeResult = pbouterfec::detail::kWirehairV2NeedMore;
    int recoverResult = pbouterfec::detail::kWirehairV2Success;
    WirehairV2ProfileFields profileFields{
        pbouterfec::kWirehairV2CertifiedProfileId,
        117,
        16,
        0};
    std::uint32_t serializedProfileBytesOut = 32;
    std::uint32_t encodeBytesOut = 16;
    std::uint64_t recoverBytesOut = 117;
    bool returnHandleOnFailure = false;
    std::uint32_t encoderCreateProfileIdCount = 0;
    std::uint32_t encoderCreateProfileCount = 0;
    std::uint32_t decoderCreateCount = 0;
    std::uint32_t encodeCount = 0;
    std::uint32_t decodeCount = 0;
    std::uint32_t recoverCount = 0;
    std::uint32_t freeCount = 0;
};

thread_local FakeBackendState* activeFakeState = nullptr;

class ActiveFakeState
{
public:
    explicit ActiveFakeState(FakeBackendState& state) noexcept
        : previousState_(activeFakeState)
    {
        activeFakeState = &state;
    }

    ActiveFakeState(const ActiveFakeState&) = delete;
    ActiveFakeState& operator=(const ActiveFakeState&) = delete;

    ~ActiveFakeState()
    {
        activeFakeState = previousState_;
    }

private:
    FakeBackendState* previousState_ = nullptr;
};

[[nodiscard]] int FakeProfileValidate(const void*, const std::uint32_t)
{
    return activeFakeState == nullptr
        ? pbouterfec::detail::kWirehairV2Error
        : activeFakeState->profileValidateResult;
}

[[nodiscard]] int FakeProfileDeserialize(
    const void*,
    const std::uint32_t,
    WirehairV2ProfileFields* const profileFields)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    if (activeFakeState->profileDeserializeResult
        == pbouterfec::detail::kWirehairV2Success
        && profileFields != nullptr)
    {
        *profileFields = activeFakeState->profileFields;
    }
    return activeFakeState->profileDeserializeResult;
}

[[nodiscard]] void* FakeHandle() noexcept
{
    return static_cast<void*>(activeFakeState);
}

void SetFakeHandle(
    const int result,
    void** const codecOut)
{
    if (codecOut == nullptr || activeFakeState == nullptr)
    {
        return;
    }
    *codecOut = result == pbouterfec::detail::kWirehairV2Success
        || activeFakeState->returnHandleOnFailure
        ? FakeHandle()
        : nullptr;
}

[[nodiscard]] int FakeEncoderCreateProfileId(
    const std::uint64_t,
    const void*,
    const std::uint64_t,
    const std::uint32_t,
    void* const serializedProfileOut,
    const std::uint32_t serializedProfileCapacity,
    std::uint32_t* const serializedProfileBytesOut,
    void** const codecOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->encoderCreateProfileIdCount++;
    if (serializedProfileBytesOut != nullptr)
    {
        *serializedProfileBytesOut =
            activeFakeState->serializedProfileBytesOut;
    }
    if (activeFakeState->encoderCreateProfileIdResult
        == pbouterfec::detail::kWirehairV2Success
        && serializedProfileOut != nullptr
        && serializedProfileCapacity >= kCanonicalProfile.size())
    {
        std::memcpy(
            serializedProfileOut,
            kCanonicalProfile.data(),
            kCanonicalProfile.size());
    }
    SetFakeHandle(
        activeFakeState->encoderCreateProfileIdResult, codecOut);
    return activeFakeState->encoderCreateProfileIdResult;
}

[[nodiscard]] int FakeEncoderCreateProfile(
    const void*,
    const void*,
    const std::uint32_t,
    void** const codecOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->encoderCreateProfileCount++;
    SetFakeHandle(activeFakeState->encoderCreateProfileResult, codecOut);
    return activeFakeState->encoderCreateProfileResult;
}

[[nodiscard]] int FakeDecoderCreate(
    const void*,
    const std::uint32_t,
    void** const codecOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->decoderCreateCount++;
    SetFakeHandle(activeFakeState->decoderCreateResult, codecOut);
    return activeFakeState->decoderCreateResult;
}

[[nodiscard]] int FakeEncode(
    void*,
    const std::uint32_t,
    void* const blockDataOut,
    const std::uint32_t outputCapacity,
    std::uint32_t* const dataBytesOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->encodeCount++;
    if (dataBytesOut != nullptr)
    {
        *dataBytesOut = activeFakeState->encodeBytesOut;
    }
    if (activeFakeState->encodeResult
        == pbouterfec::detail::kWirehairV2Success
        && blockDataOut != nullptr)
    {
        const std::uint32_t bytesToWrite = std::min(
            outputCapacity, activeFakeState->encodeBytesOut);
        std::fill_n(
            static_cast<std::byte*>(blockDataOut),
            bytesToWrite,
            Byte(0x5C));
    }
    return activeFakeState->encodeResult;
}

[[nodiscard]] int FakeDecode(
    void*,
    const std::uint32_t,
    const void*,
    const std::uint32_t)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->decodeCount++;
    return activeFakeState->decodeResult;
}

[[nodiscard]] int FakeRecover(
    void*,
    void* const messageOut,
    const std::uint64_t outputCapacity,
    std::uint64_t* const bytesOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->recoverCount++;
    if (bytesOut != nullptr)
    {
        *bytesOut = activeFakeState->recoverBytesOut;
    }
    if (activeFakeState->recoverResult
        == pbouterfec::detail::kWirehairV2Success
        && messageOut != nullptr)
    {
        const std::uint64_t bytesToWrite = std::min(
            outputCapacity, activeFakeState->recoverBytesOut);
        std::fill_n(
            static_cast<std::byte*>(messageOut),
            static_cast<std::size_t>(bytesToWrite),
            Byte(0x6D));
    }
    return activeFakeState->recoverResult;
}

void FakeFree(void*)
{
    if (activeFakeState != nullptr)
    {
        activeFakeState->freeCount++;
    }
}

[[nodiscard]] WirehairV2Backend MakeFakeBackend() noexcept
{
    return WirehairV2Backend{
        &FakeProfileValidate,
        &FakeProfileDeserialize,
        &FakeEncoderCreateProfileId,
        &FakeEncoderCreateProfile,
        &FakeDecoderCreate,
        &FakeEncode,
        &FakeDecode,
        &FakeRecover,
        &FakeFree};
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeFakeDescriptor()
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = 117;
    descriptor.outerBlockBytes = 16;
    descriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    descriptor.wirehairV2SerializedProfile =
        pbprotocol::WirehairV2SerializedProfile{kCanonicalProfile};
    return descriptor;
}

} // namespace

TEST_CASE("Wirehair V2 stable create errors are mapped and failure handles are freed")
{
    struct ErrorCase
    {
        int result = pbouterfec::detail::kWirehairV2Error;
        pbouterfec::OuterFecErrorCode expectedError =
            pbouterfec::OuterFecErrorCode::CodecError;
    };
    const std::array<ErrorCase, 14> errorCases{{
        {pbouterfec::detail::kWirehairV2InvalidInput,
            pbouterfec::OuterFecErrorCode::InvalidInput},
        {pbouterfec::detail::kWirehairV2BufferTooSmall,
            pbouterfec::OuterFecErrorCode::BufferTooSmall},
        {pbouterfec::detail::kWirehairV2InvalidMagic,
            pbouterfec::OuterFecErrorCode::InvalidMagic},
        {pbouterfec::detail::kWirehairV2UnsupportedVersion,
            pbouterfec::OuterFecErrorCode::UnsupportedVersion},
        {pbouterfec::detail::kWirehairV2InvalidSize,
            pbouterfec::OuterFecErrorCode::InvalidSize},
        {pbouterfec::detail::kWirehairV2ReservedNonzero,
            pbouterfec::OuterFecErrorCode::ReservedNonzero},
        {pbouterfec::detail::kWirehairV2UnsupportedProfile,
            pbouterfec::OuterFecErrorCode::UnsupportedProfile},
        {pbouterfec::detail::kWirehairV2InvalidDimensions,
            pbouterfec::OuterFecErrorCode::InvalidDimensions},
        {pbouterfec::detail::kWirehairV2BadSeed,
            pbouterfec::OuterFecErrorCode::BadSeed},
        {pbouterfec::detail::kWirehairV2ExtraInsufficient,
            pbouterfec::OuterFecErrorCode::ExtraInsufficient},
        {pbouterfec::detail::kWirehairV2Error,
            pbouterfec::OuterFecErrorCode::CodecError},
        {pbouterfec::detail::kWirehairV2OutOfMemory,
            pbouterfec::OuterFecErrorCode::OutOfMemory},
        {pbouterfec::detail::kWirehairV2UnsupportedPlatform,
            pbouterfec::OuterFecErrorCode::UnsupportedPlatform},
        {777, pbouterfec::OuterFecErrorCode::CodecError}}};

    for (const ErrorCase& errorCase : errorCases)
    {
        CAPTURE(errorCase.result);
        FakeBackendState state;
        state.decoderCreateResult = errorCase.result;
        state.returnHandleOnFailure = true;
        ActiveFakeState activeState(state);
        const WirehairV2Backend backend = MakeFakeBackend();
        pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(
            backend);

        const auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            MakeFakeDescriptor());
        REQUIRE_FALSE(decoderResult);
        REQUIRE(decoderResult.Error().code == errorCase.expectedError);
        if (errorCase.result == 777)
        {
            REQUIRE(decoderResult.Error().detail == 777);
        }
        REQUIRE(state.decoderCreateCount == 1);
        REQUIRE(state.freeCount == 1);
    }
}

TEST_CASE("Wirehair V2 NeedMore is normal and ExtraInsufficient is terminal")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    {
        auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            MakeFakeDescriptor());
        REQUIRE(decoderResult);
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        const std::array<std::byte, 16> payload{};

        const auto needMoreResult = decoder.DecodeBlock(0, payload);
        REQUIRE(needMoreResult);
        REQUIRE(needMoreResult.Value()
            == pbouterfec::DecodeDisposition::NeedMore);
        REQUIRE(state.decodeCount == 1);

        const auto duplicateResult = decoder.DecodeBlock(0, payload);
        REQUIRE(duplicateResult);
        REQUIRE(duplicateResult.Value()
            == pbouterfec::DecodeDisposition::NeedMore);
        REQUIRE(state.decodeCount == 1);

        state.decodeResult = pbouterfec::detail::kWirehairV2ExtraInsufficient;
        const auto exhaustedResult = decoder.DecodeBlock(1, payload);
        REQUIRE_FALSE(exhaustedResult);
        REQUIRE(exhaustedResult.Error().code
            == pbouterfec::OuterFecErrorCode::ExtraInsufficient);
        REQUIRE(state.decodeCount == 2);

        const auto afterExhaustion = decoder.DecodeBlock(2, payload);
        REQUIRE_FALSE(afterExhaustion);
        REQUIRE(afterExhaustion.Error().code
            == pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(state.decodeCount == 2);
    }
    REQUIRE(state.freeCount == 1);
}

TEST_CASE("Wirehair V2 wrapper enforces a finite accepted-ID window")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
        MakeFakeDescriptor());
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();
    const std::array<std::byte, 16> payload{};
    constexpr std::uint32_t blockCount = 8;
    constexpr std::uint32_t maximumAcceptedIds = blockCount + 1024U;

    for (std::uint32_t blockOffset = 0;
        blockOffset < maximumAcceptedIds;
        blockOffset++)
    {
        const auto decodeResult = decoder.DecodeBlock(
            blockCount + blockOffset, payload);
        REQUIRE(decodeResult);
        REQUIRE(decodeResult.Value()
            == pbouterfec::DecodeDisposition::NeedMore);
    }
    REQUIRE(state.decodeCount == maximumAcceptedIds);

    const auto exhaustedResult = decoder.DecodeBlock(
        blockCount + maximumAcceptedIds, payload);
    REQUIRE_FALSE(exhaustedResult);
    REQUIRE(exhaustedResult.Error().code
        == pbouterfec::OuterFecErrorCode::ExtraInsufficient);
    REQUIRE(exhaustedResult.Error().detail == maximumAcceptedIds);
    REQUIRE(state.decodeCount == maximumAcceptedIds);

    const auto afterExhaustion = decoder.DecodeBlock(
        blockCount + maximumAcceptedIds + 1U, payload);
    REQUIRE_FALSE(afterExhaustion);
    REQUIRE(afterExhaustion.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidState);
    REQUIRE(state.decodeCount == maximumAcceptedIds);
}

TEST_CASE("Wirehair V2 fake recover BufferTooSmall is retriable")
{
    FakeBackendState state;
    state.decodeResult = pbouterfec::detail::kWirehairV2Success;
    state.recoverResult = pbouterfec::detail::kWirehairV2BufferTooSmall;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
        MakeFakeDescriptor());
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();
    const std::array<std::byte, 16> payload{};
    const auto decodeResult = decoder.DecodeBlock(0, payload);
    REQUIRE(decodeResult);
    REQUIRE(decodeResult.Value() == pbouterfec::DecodeDisposition::Ready);

    std::vector<std::byte> output(117, Byte(0xA7));
    const std::vector<std::byte> originalOutput = output;
    const auto shortResult = decoder.Recover(output);
    REQUIRE_FALSE(shortResult);
    REQUIRE(shortResult.Error().code
        == pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortResult.Error().detail == 117);
    REQUIRE(output == originalOutput);

    state.recoverResult = pbouterfec::detail::kWirehairV2Success;
    const auto retryResult = decoder.Recover(output);
    REQUIRE(retryResult);
    REQUIRE(retryResult.Value() == 117);
    REQUIRE(state.recoverCount == 2);
}

TEST_CASE("Wirehair V2 fake encode BufferTooSmall is transactional and retriable")
{
    FakeBackendState state;
    state.encodeResult = pbouterfec::detail::kWirehairV2BufferTooSmall;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);
    const std::vector<std::byte> message(117, Byte(0x31));

    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(message, 16);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    std::array<std::byte, 16> output{};
    output.fill(Byte(0xE3));
    const std::array<std::byte, 16> originalOutput = output;

    const auto shortResult = encoder.EncodeBlock(0, output);
    REQUIRE_FALSE(shortResult);
    REQUIRE(shortResult.Error().code
        == pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortResult.Error().detail == 16);
    REQUIRE(output == originalOutput);

    state.encodeResult = pbouterfec::detail::kWirehairV2Success;
    const auto retryResult = encoder.EncodeBlock(0, output);
    REQUIRE(retryResult);
    REQUIRE(retryResult.Value() == 16);
    REQUIRE(state.encodeCount == 2);
}

TEST_CASE("Wirehair V2 digest mismatch precedes both recreation create calls")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    std::vector<std::byte> exactMessage(117, Byte(0x41));
    pbprotocol::SegmentDescriptor descriptor = MakeFakeDescriptor();
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(exactMessage)};

    std::vector<std::byte> changedMessage = exactMessage;
    changedMessage[53] ^= Byte(0x01);
    const auto mismatchResult = pbouterfec::WirehairV2Encoder::Recreate(
        changedMessage, descriptor);
    REQUIRE_FALSE(mismatchResult);
    REQUIRE(mismatchResult.Error().code
        == pbouterfec::OuterFecErrorCode::EncodedDigestMismatch);
    REQUIRE(state.encoderCreateProfileIdCount == 0);
    REQUIRE(state.encoderCreateProfileCount == 0);

    {
        const auto exactResult = pbouterfec::WirehairV2Encoder::Recreate(
            exactMessage, descriptor);
        REQUIRE(exactResult);
    }
    REQUIRE(state.encoderCreateProfileIdCount == 1);
    REQUIRE(state.encoderCreateProfileCount == 1);
    REQUIRE(state.freeCount == 2);
}

TEST_CASE("Wirehair V2 failed creation and moves release each fake handle once")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);
    const std::vector<std::byte> message(117, Byte(0x21));

    state.encoderCreateProfileIdResult =
        pbouterfec::detail::kWirehairV2OutOfMemory;
    state.returnHandleOnFailure = true;
    const auto failedResult = pbouterfec::WirehairV2Encoder::Create(
        message, 16);
    REQUIRE_FALSE(failedResult);
    REQUIRE(failedResult.Error().code
        == pbouterfec::OuterFecErrorCode::OutOfMemory);
    REQUIRE(state.freeCount == 1);

    state.encoderCreateProfileIdResult =
        pbouterfec::detail::kWirehairV2Success;
    state.returnHandleOnFailure = false;
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
            message, 16);
        REQUIRE(encoderResult);
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        pbouterfec::WirehairV2Encoder movedEncoder = std::move(encoder);

        std::array<std::byte, 16> output{};
        const auto movedFromResult = encoder.EncodeBlock(0, output);
        REQUIRE_FALSE(movedFromResult);
        REQUIRE(movedFromResult.Error().code
            == pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(movedEncoder.EncodeBlock(0, output));
    }
    REQUIRE(state.freeCount == 2);
    REQUIRE(state.encodeCount == 1);

    {
        auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            MakeFakeDescriptor());
        REQUIRE(decoderResult);
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        pbouterfec::WirehairV2Decoder movedDecoder = std::move(decoder);
        const std::array<std::byte, 16> payload{};
        const auto movedFromResult = decoder.DecodeBlock(0, payload);
        REQUIRE_FALSE(movedFromResult);
        REQUIRE(movedFromResult.Error().code
            == pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(movedDecoder.DecodeBlock(0, payload));
    }
    REQUIRE(state.freeCount == 3);
}

TEST_CASE("Wirehair V2 fake invalid and unknown decode results fail closed")
{
    struct DecodeErrorCase
    {
        int result = pbouterfec::detail::kWirehairV2Error;
        pbouterfec::OuterFecErrorCode expectedError =
            pbouterfec::OuterFecErrorCode::CodecError;
    };
    const std::array<DecodeErrorCase, 7> errorCases{{
        {pbouterfec::detail::kWirehairV2InvalidInput,
            pbouterfec::OuterFecErrorCode::InvalidInput},
        {pbouterfec::detail::kWirehairV2BadSeed,
            pbouterfec::OuterFecErrorCode::BadSeed},
        {pbouterfec::detail::kWirehairV2OutOfMemory,
            pbouterfec::OuterFecErrorCode::OutOfMemory},
        {pbouterfec::detail::kWirehairV2UnsupportedProfile,
            pbouterfec::OuterFecErrorCode::UnsupportedProfile},
        {pbouterfec::detail::kWirehairV2UnsupportedPlatform,
            pbouterfec::OuterFecErrorCode::UnsupportedPlatform},
        {pbouterfec::detail::kWirehairV2Error,
            pbouterfec::OuterFecErrorCode::CodecError},
        {991, pbouterfec::OuterFecErrorCode::CodecError}}};

    for (const DecodeErrorCase& errorCase : errorCases)
    {
        CAPTURE(errorCase.result);
        FakeBackendState state;
        state.decodeResult = errorCase.result;
        ActiveFakeState activeState(state);
        const WirehairV2Backend backend = MakeFakeBackend();
        pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(
            backend);

        auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            MakeFakeDescriptor());
        REQUIRE(decoderResult);
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        const std::array<std::byte, 16> payload{};
        const auto decodeResult = decoder.DecodeBlock(0, payload);
        REQUIRE_FALSE(decodeResult);
        REQUIRE(decodeResult.Error().code == errorCase.expectedError);
        if (errorCase.result == 991)
        {
            REQUIRE(decodeResult.Error().detail == 991);
        }

        const auto afterFailure = decoder.DecodeBlock(1, payload);
        REQUIRE_FALSE(afterFailure);
        REQUIRE(afterFailure.Error().code
            == pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(state.decodeCount == 1);
    }
}
