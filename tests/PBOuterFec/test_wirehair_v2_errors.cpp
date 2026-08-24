#include "pbouterfec/wirehair_v2.h"

#include "decoder_test_access.h"

#include "pbprotocol/blake3_digest.h"
#include "wirehair_v2_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

[[nodiscard]] pbouterfec::WirehairV2DecoderResourceManager
MakeDecoderResourceManager(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy())
{
    auto resourceManagerResult =
        pbouterfec::WirehairV2DecoderResourceManager::Create(resourcePolicy);
    REQUIRE(resourceManagerResult);
    return std::move(resourceManagerResult).Value();
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
    std::vector<std::byte> recreatedMessage;
    std::array<std::byte, 32> recreatedProfile{};
    std::uint32_t recreatedProfileBytes = 0;
    std::array<std::byte, 32> decoderProfile{};
    std::uint32_t decoderProfileBytes = 0;
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
    const void* const message,
    const void* const serializedProfile,
    const std::uint32_t serializedProfileBytes,
    void** const codecOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->encoderCreateProfileCount++;
    if (message != nullptr)
    {
        const auto* const messageBytes = static_cast<const std::byte*>(message);
        activeFakeState->recreatedMessage.assign(
            messageBytes,
            messageBytes + static_cast<std::size_t>(
                activeFakeState->profileFields.messageBytes));
    }
    activeFakeState->recreatedProfileBytes = serializedProfileBytes;
    if (serializedProfile != nullptr &&
        serializedProfileBytes == static_cast<std::uint32_t>(
            activeFakeState->recreatedProfile.size()))
    {
        std::memcpy(
            activeFakeState->recreatedProfile.data(),
            serializedProfile,
            activeFakeState->recreatedProfile.size());
    }
    SetFakeHandle(activeFakeState->encoderCreateProfileResult, codecOut);
    return activeFakeState->encoderCreateProfileResult;
}

[[nodiscard]] int FakeDecoderCreate(
    const void* const serializedProfile,
    const std::uint32_t serializedProfileBytes,
    void** const codecOut)
{
    if (activeFakeState == nullptr)
    {
        return pbouterfec::detail::kWirehairV2Error;
    }
    activeFakeState->decoderCreateCount++;
    activeFakeState->decoderProfileBytes = serializedProfileBytes;
    if (serializedProfile != nullptr &&
        serializedProfileBytes == static_cast<std::uint32_t>(
            activeFakeState->decoderProfile.size()))
    {
        std::memcpy(
            activeFakeState->decoderProfile.data(),
            serializedProfile,
            activeFakeState->decoderProfile.size());
    }
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
    std::array<std::byte, 117> recoveredMessage{};
    recoveredMessage.fill(Byte(0x6D));
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(recoveredMessage)};
    descriptor.wirehairV2SerializedProfile =
        pbprotocol::WirehairV2SerializedProfile{kCanonicalProfile};
    return descriptor;
}

[[nodiscard]] std::vector<std::uint32_t> MakeCollidingOuterBlockIds(
    const std::size_t count,
    const std::uint32_t bucketMask,
    const std::uint64_t hashSalt)
{
    std::vector<std::uint32_t> blockIds;
    blockIds.reserve(count);
    for (std::uint64_t candidate = 8;
        candidate <= std::numeric_limits<std::uint32_t>::max() &&
            blockIds.size() < count;
        candidate++)
    {
        const std::uint32_t outerBlockId =
            static_cast<std::uint32_t>(candidate);
        if ((pbouterfec::detail::HashWirehairV2AcceptedBlockId(
                outerBlockId, hashSalt) & bucketMask) == 0)
        {
            blockIds.push_back(outerBlockId);
        }
    }
    return blockIds;
}

} // namespace

TEST_CASE("Wirehair V2 stable create errors are mapped and failure handles are freed")
{
    auto resourceManager = MakeDecoderResourceManager();
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

        const auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
        REQUIRE_FALSE(decoderResult);
        REQUIRE(decoderResult.Error().code == errorCase.expectedError);
        if (errorCase.result == 777)
        {
            REQUIRE(decoderResult.Error().detail == 777);
        }
        REQUIRE(state.decoderCreateCount == 1);
        REQUIRE(state.decoderProfile == kCanonicalProfile);
        REQUIRE(state.decoderProfileBytes ==
            static_cast<std::uint32_t>(kCanonicalProfile.size()));
        REQUIRE(state.freeCount == 1);
    }
}

TEST_CASE("Wirehair V2 successful backend calls must report exact byte counts")
{
    auto resourceManager = MakeDecoderResourceManager();
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);
    const std::vector<std::byte> message(117, Byte(0x27));

    state.serializedProfileBytesOut = 31;
    const auto shortProfileResult = pbouterfec::WirehairV2Encoder::Create(
        message, 16);
    REQUIRE_FALSE(shortProfileResult);
    REQUIRE(shortProfileResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidSize);
    REQUIRE(shortProfileResult.Error().detail == 31);
    REQUIRE(state.freeCount == 1);

    state.serializedProfileBytesOut = 32;
    state.encodeBytesOut = 15;
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Create(message, 16);
        REQUIRE(encoderResult);
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        std::array<std::byte, 16> output{};
        const auto encodeResult = encoder.EncodeBlock(0, output);
        REQUIRE_FALSE(encodeResult);
        REQUIRE(encodeResult.Error().code ==
            pbouterfec::OuterFecErrorCode::CodecError);
        REQUIRE(encodeResult.Error().detail == 15);
        const auto afterEncodeFailure = encoder.EncodeBlock(1, output);
        REQUIRE_FALSE(afterEncodeFailure);
        REQUIRE(afterEncodeFailure.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(state.encodeCount == 1);
    }
    REQUIRE(state.freeCount == 2);

    state.decodeResult = pbouterfec::detail::kWirehairV2Success;
    state.recoverBytesOut = 116;
    {
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
        REQUIRE(decoderResult);
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        const std::array<std::byte, 16> payload{};
        REQUIRE(decoder.DecodeBlock(0, payload));

        std::vector<std::byte> output(117);
        const auto recoverResult = decoder.Recover(output);
        REQUIRE_FALSE(recoverResult);
        REQUIRE(recoverResult.Error().code ==
            pbouterfec::OuterFecErrorCode::CodecError);
        REQUIRE(recoverResult.Error().detail == 116);
        const auto afterRecoverFailure = decoder.Recover(output);
        REQUIRE_FALSE(afterRecoverFailure);
        REQUIRE(afterRecoverFailure.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidState);
        REQUIRE(state.recoverCount == 1);
    }
    REQUIRE(state.freeCount == 3);
    REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
    REQUIRE(resourceManager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("Wirehair V2 decoder rejects invalid and exceeded resource policies before backend creation")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    const auto invalidManagerResult =
        pbouterfec::WirehairV2DecoderResourceManager::Create({});
    REQUIRE_FALSE(invalidManagerResult);
    REQUIRE(invalidManagerResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidResourcePolicy);
    REQUIRE(state.decoderCreateCount == 0);

    pbprotocol::ReceiverResourcePolicy encodedPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    encodedPolicy.maxEncodedSegmentBytes = 116;
    auto encodedManager = MakeDecoderResourceManager(encodedPolicy);
    const auto encodedLimitResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), encodedManager);
    REQUIRE_FALSE(encodedLimitResult);
    REQUIRE(encodedLimitResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(encodedLimitResult.Error().detail == 117);
    REQUIRE(state.decoderCreateCount == 0);

    pbprotocol::ReceiverResourcePolicy blockPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    blockPolicy.maxOuterBlockBytes = 15;
    auto blockManager = MakeDecoderResourceManager(blockPolicy);
    const auto blockLimitResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), blockManager);
    REQUIRE_FALSE(blockLimitResult);
    REQUIRE(blockLimitResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(blockLimitResult.Error().detail == 16);
    REQUIRE(state.decoderCreateCount == 0);
}

TEST_CASE("Wirehair V2 decoder reservations are inclusive and released exactly once")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto probeManager = MakeDecoderResourceManager();
    std::uint64_t reservationBytes = 0;
    {
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), probeManager);
        REQUIRE(decoderResult);
        reservationBytes = probeManager.GetReservedDecoderBytes();
        // K=8, B=16: this includes the pinned backend's 24-byte
        // ReceivedPacketRecord for all 4096 accepted-ID slots. Keeping this
        // exact value makes an accidental return to the old 16-byte estimate
        // visible rather than silently weakening the receiver budget.
        REQUIRE(reservationBytes == 1641172ULL);
        REQUIRE(probeManager.GetActiveDecoderCount() == 1);
    }
    REQUIRE(probeManager.GetActiveDecoderCount() == 0);
    REQUIRE(probeManager.GetReservedDecoderBytes() == 0);

    pbprotocol::ReceiverResourcePolicy belowPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    belowPolicy.maxOuterFecDecoderBytes = reservationBytes - 1ULL;
    auto belowManager = MakeDecoderResourceManager(belowPolicy);
    const std::uint32_t backendCreatesBeforeRefusal = state.decoderCreateCount;
    const auto belowResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), belowManager);
    REQUIRE_FALSE(belowResult);
    REQUIRE(belowResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(state.decoderCreateCount == backendCreatesBeforeRefusal);
    REQUIRE(belowManager.GetActiveDecoderCount() == 0);
    REQUIRE(belowManager.GetReservedDecoderBytes() == 0);

    pbprotocol::ReceiverResourcePolicy exactPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    exactPolicy.maxActiveOuterFecDecoders = 1;
    exactPolicy.maxOuterFecDecoderBytes = reservationBytes;
    exactPolicy.maxTotalOuterFecDecoderBytes = reservationBytes;
    auto exactManager = MakeDecoderResourceManager(exactPolicy);
    {
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), exactManager);
        REQUIRE(decoderResult);
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        REQUIRE(exactManager.GetActiveDecoderCount() == 1);
        REQUIRE(exactManager.GetReservedDecoderBytes() == reservationBytes);

        const std::uint32_t backendCreatesAtLimit = state.decoderCreateCount;
        const auto countLimitResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), exactManager);
        REQUIRE_FALSE(countLimitResult);
        REQUIRE(countLimitResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        REQUIRE(state.decoderCreateCount == backendCreatesAtLimit);

        pbouterfec::WirehairV2Decoder movedDecoder = std::move(decoder);
        REQUIRE(exactManager.GetActiveDecoderCount() == 1);
        const std::array<std::byte, 16> payload{};
        REQUIRE(movedDecoder.DecodeBlock(0, payload));
    }
    REQUIRE(exactManager.GetActiveDecoderCount() == 0);
    REQUIRE(exactManager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("Wirehair V2 aggregate reservation and failed backend creation roll back")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto probeManager = MakeDecoderResourceManager();
    std::uint64_t reservationBytes = 0;
    {
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), probeManager);
        REQUIRE(decoderResult);
        reservationBytes = probeManager.GetReservedDecoderBytes();
    }

    pbprotocol::ReceiverResourcePolicy aggregatePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    aggregatePolicy.maxActiveOuterFecDecoders = 3;
    aggregatePolicy.maxOuterFecDecoderBytes = reservationBytes;
    aggregatePolicy.maxTotalOuterFecDecoderBytes = reservationBytes * 2ULL;
    auto aggregateManager = MakeDecoderResourceManager(aggregatePolicy);
    {
        auto firstResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), aggregateManager);
        auto secondResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), aggregateManager);
        REQUIRE(firstResult);
        REQUIRE(secondResult);
        REQUIRE(aggregateManager.GetActiveDecoderCount() == 2);
        REQUIRE(aggregateManager.GetReservedDecoderBytes() ==
            reservationBytes * 2ULL);

        const std::uint32_t backendCreatesAtLimit = state.decoderCreateCount;
        const auto aggregateLimitResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), aggregateManager);
        REQUIRE_FALSE(aggregateLimitResult);
        REQUIRE(aggregateLimitResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        REQUIRE(state.decoderCreateCount == backendCreatesAtLimit);
    }
    REQUIRE(aggregateManager.GetActiveDecoderCount() == 0);
    REQUIRE(aggregateManager.GetReservedDecoderBytes() == 0);

    auto failureManager = MakeDecoderResourceManager();
    state.decoderCreateResult = pbouterfec::detail::kWirehairV2OutOfMemory;
    state.returnHandleOnFailure = true;
    const std::uint32_t freesBeforeFailure = state.freeCount;
    const auto failedResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), failureManager);
    REQUIRE_FALSE(failedResult);
    REQUIRE(failedResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OutOfMemory);
    REQUIRE(state.freeCount == freesBeforeFailure + 1U);
    REQUIRE(failureManager.GetActiveDecoderCount() == 0);
    REQUIRE(failureManager.GetReservedDecoderBytes() == 0);

    state.decoderCreateResult = pbouterfec::detail::kWirehairV2Success;
    state.returnHandleOnFailure = false;
    {
        const auto retryResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), failureManager);
        REQUIRE(retryResult);
        REQUIRE(failureManager.GetActiveDecoderCount() == 1);
    }
    REQUIRE(failureManager.GetActiveDecoderCount() == 0);
    REQUIRE(failureManager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("Wirehair V2 decoder reservation safely outlives its manager")
{
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    std::optional<pbouterfec::WirehairV2Decoder> decoder;
    {
        auto resourceManager = MakeDecoderResourceManager();
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
        REQUIRE(decoderResult);
        decoder.emplace(std::move(decoderResult).Value());
        REQUIRE(resourceManager.GetActiveDecoderCount() == 1);
        REQUIRE(resourceManager.GetReservedDecoderBytes() > 0);
    }

    const std::array<std::byte, 16> payload{};
    REQUIRE(decoder->DecodeBlock(0, payload));
    REQUIRE(state.decodeCount == 1);
    REQUIRE(state.freeCount == 0);
    decoder.reset();
    REQUIRE(state.freeCount == 1);
}

TEST_CASE("Wirehair V2 NeedMore is normal and ExtraInsufficient is terminal")
{
    auto resourceManager = MakeDecoderResourceManager();
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    {
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
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
    auto resourceManager = MakeDecoderResourceManager();
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), resourceManager);
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

TEST_CASE("Wirehair V2 independently salted accepted-ID table bounds local probing")
{
    constexpr std::uint64_t hashSalt = 0x6A09E667F3BCC909ULL;
    auto resourceManager = MakeDecoderResourceManager();
    FakeBackendState state;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);
    pbouterfec::detail::ScopedWirehairV2AcceptedBlockHashSaltOverride
        overrideHashSalt(hashSalt);

    auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), resourceManager);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();

    // K=8 provisions an initial 128-slot wrapper table. The deterministic test
    // salt lets this test prove the wrapper's independent 64-probe bound; it
    // deliberately makes no claim about Wirehair's separately salted table.
    const std::vector<std::uint32_t> collidingIds =
        MakeCollidingOuterBlockIds(65, 127U, hashSalt);
    REQUIRE(collidingIds.size() == 65);
    const std::array<std::byte, 16> payload{};

    for (std::size_t idIndex = 0; idIndex < 64; idIndex++)
    {
        const auto decodeResult = decoder.DecodeBlock(
            collidingIds[idIndex], payload);
        REQUIRE(decodeResult);
        REQUIRE(decodeResult.Value() ==
            pbouterfec::DecodeDisposition::NeedMore);
    }
    REQUIRE(state.decodeCount == 64);

    const auto boundedFailure = decoder.DecodeBlock(
        collidingIds[64], payload);
    REQUIRE_FALSE(boundedFailure);
    REQUIRE(boundedFailure.Error().code ==
        pbouterfec::OuterFecErrorCode::ExtraInsufficient);
    REQUIRE(boundedFailure.Error().detail == 64);
    REQUIRE(state.decodeCount == 64);

    const auto afterFailure = decoder.DecodeBlock(
        collidingIds[0], payload);
    REQUIRE_FALSE(afterFailure);
    REQUIRE(afterFailure.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);
    REQUIRE(state.decodeCount == 64);
}

TEST_CASE("Wirehair V2 fake recover BufferTooSmall is retriable")
{
    auto resourceManager = MakeDecoderResourceManager();
    FakeBackendState state;
    state.decodeResult = pbouterfec::detail::kWirehairV2Success;
    state.recoverResult = pbouterfec::detail::kWirehairV2BufferTooSmall;
    ActiveFakeState activeState(state);
    const WirehairV2Backend backend = MakeFakeBackend();
    pbouterfec::detail::ScopedWirehairV2BackendOverride overrideBackend(backend);

    auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
        MakeFakeDescriptor(), resourceManager);
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

TEST_CASE("Wirehair V2 recreation uses only exact saved profile and message")
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
    REQUIRE(state.encoderCreateProfileIdCount == 0);
    REQUIRE(state.encoderCreateProfileCount == 1);
    REQUIRE(state.recreatedMessage == exactMessage);
    REQUIRE(state.recreatedProfile == kCanonicalProfile);
    REQUIRE(state.recreatedProfileBytes ==
        static_cast<std::uint32_t>(kCanonicalProfile.size()));
    REQUIRE(state.freeCount == 1);
}

TEST_CASE("Wirehair V2 failed creation and moves release each fake handle once")
{
    auto resourceManager = MakeDecoderResourceManager();
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
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
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
    auto resourceManager = MakeDecoderResourceManager();
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

        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            MakeFakeDescriptor(), resourceManager);
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
