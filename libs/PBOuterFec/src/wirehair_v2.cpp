#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"
#include "wirehair_v2_backend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace pbouterfec {
namespace {

using detail::WirehairV2Backend;
using detail::WirehairV2ProfileFields;

inline constexpr std::uint32_t kWirehairV2MaximumBlockBytes = 0x7FFFFFFFU;
inline constexpr std::uint64_t kWirehairV2RetainedExtraIds = 1024;

static_assert(
    kWirehairV2CertifiedProfileId ==
    pbprotocol::kWirehairV2CertifiedProfileId);
static_assert(
    pbprotocol::kWirehairV2SerializedProfileBytes == 32);

[[nodiscard]] std::uint64_t RawResultDetail(const int result) noexcept
{
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(result));
}

[[nodiscard]] OuterFecError MapWirehairFailure(
    const int result,
    const std::uint64_t errorDetail = 0) noexcept
{
    switch (result)
    {
    case detail::kWirehairV2InvalidInput:
        return OuterFecError{OuterFecErrorCode::InvalidInput, errorDetail};
    case detail::kWirehairV2BufferTooSmall:
        return OuterFecError{OuterFecErrorCode::BufferTooSmall, errorDetail};
    case detail::kWirehairV2InvalidMagic:
        return OuterFecError{OuterFecErrorCode::InvalidMagic, errorDetail};
    case detail::kWirehairV2UnsupportedVersion:
        return OuterFecError{OuterFecErrorCode::UnsupportedVersion, errorDetail};
    case detail::kWirehairV2InvalidSize:
        return OuterFecError{OuterFecErrorCode::InvalidSize, errorDetail};
    case detail::kWirehairV2ReservedNonzero:
        return OuterFecError{OuterFecErrorCode::ReservedNonzero, errorDetail};
    case detail::kWirehairV2UnsupportedProfile:
        return OuterFecError{OuterFecErrorCode::UnsupportedProfile, errorDetail};
    case detail::kWirehairV2InvalidDimensions:
        return OuterFecError{OuterFecErrorCode::InvalidDimensions, errorDetail};
    case detail::kWirehairV2BadSeed:
        return OuterFecError{OuterFecErrorCode::BadSeed, errorDetail};
    case detail::kWirehairV2ExtraInsufficient:
        return OuterFecError{OuterFecErrorCode::ExtraInsufficient, errorDetail};
    case detail::kWirehairV2Error:
        return OuterFecError{OuterFecErrorCode::CodecError, errorDetail};
    case detail::kWirehairV2OutOfMemory:
        return OuterFecError{OuterFecErrorCode::OutOfMemory, errorDetail};
    case detail::kWirehairV2UnsupportedPlatform:
        return OuterFecError{OuterFecErrorCode::UnsupportedPlatform, errorDetail};
    case detail::kWirehairV2Success:
    case detail::kWirehairV2NeedMore:
    default:
        return OuterFecError{
            OuterFecErrorCode::CodecError,
            RawResultDetail(result)};
    }
}

template <typename ValueType>
[[nodiscard]] OuterFecResult<ValueType> FailureFrom(
    const OuterFecError& error)
{
    return OuterFecResult<ValueType>::Failure(error.code, error.detail);
}

[[nodiscard]] bool IsBackendComplete(
    const WirehairV2Backend& backend) noexcept
{
    return backend.profileValidate != nullptr
        && backend.profileDeserialize != nullptr
        && backend.encoderCreateProfileId != nullptr
        && backend.encoderCreateProfile != nullptr
        && backend.decoderCreate != nullptr
        && backend.encode != nullptr
        && backend.decode != nullptr
        && backend.recover != nullptr
        && backend.freeCodec != nullptr;
}

[[nodiscard]] std::uint64_t SizeToUint64(
    const std::size_t byteCount) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        if (byteCount > std::numeric_limits<std::uint64_t>::max())
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    return static_cast<std::uint64_t>(byteCount);
}

[[nodiscard]] bool SizeFitsUint64(const std::size_t byteCount) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        return byteCount <= std::numeric_limits<std::uint64_t>::max();
    }
    return true;
}

[[nodiscard]] OuterFecResult<std::uint32_t> ValidateDimensions(
    const std::uint64_t messageBytes,
    const std::uint32_t blockBytes)
{
    if (messageBytes == 0)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidDimensions, messageBytes);
    }
    if (blockBytes == 0 || blockBytes > kWirehairV2MaximumBlockBytes)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidDimensions, blockBytes);
    }

    const std::uint64_t blockCount =
        1ULL + ((messageBytes - 1ULL) / blockBytes);
    if (blockCount < kWirehairV2MinimumBlockCount
        || blockCount > kWirehairV2MaximumBlockCount)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidDimensions, blockCount);
    }

    return OuterFecResult<std::uint32_t>::Success(
        static_cast<std::uint32_t>(blockCount));
}

[[nodiscard]] OuterFecResult<WirehairV2ProfileFields>
ValidateCanonicalProfile(
    const pbprotocol::WirehairV2SerializedProfile& serializedProfile,
    const WirehairV2Backend& backend)
{
    if (!IsBackendComplete(backend))
    {
        return OuterFecResult<WirehairV2ProfileFields>::Failure(
            OuterFecErrorCode::CodecError);
    }

    const int validateResult = backend.profileValidate(
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()));
    if (validateResult != detail::kWirehairV2Success)
    {
        return FailureFrom<WirehairV2ProfileFields>(
            MapWirehairFailure(validateResult));
    }

    WirehairV2ProfileFields profileFields{};
    const int deserializeResult = backend.profileDeserialize(
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()),
        &profileFields);
    if (deserializeResult != detail::kWirehairV2Success)
    {
        return FailureFrom<WirehairV2ProfileFields>(
            MapWirehairFailure(deserializeResult));
    }
    return OuterFecResult<WirehairV2ProfileFields>::Success(profileFields);
}

struct ValidatedDescriptorProfile
{
    const WirehairV2Backend* backend = nullptr;
    WirehairV2ProfileFields profileFields{};
    std::uint32_t blockCount = 0;
};

[[nodiscard]] OuterFecResult<ValidatedDescriptorProfile>
ValidateDescriptorProfile(
    const pbprotocol::SegmentDescriptor& segmentDescriptor)
{
    if (segmentDescriptor.outerFecMode != pbprotocol::OuterFecMode::WirehairV2
        || !segmentDescriptor.wirehairV2SerializedProfile.has_value())
    {
        return OuterFecResult<ValidatedDescriptorProfile>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            static_cast<std::uint64_t>(segmentDescriptor.outerFecMode));
    }

    const WirehairV2Backend& backend = detail::GetWirehairV2Backend();
    const auto profileResult = ValidateCanonicalProfile(
        segmentDescriptor.wirehairV2SerializedProfile.value(), backend);
    if (!profileResult)
    {
        return FailureFrom<ValidatedDescriptorProfile>(profileResult.Error());
    }
    const WirehairV2ProfileFields& profileFields = profileResult.Value();

    // The current Phase-0 production descriptor admission is intentionally
    // narrower than the dependency's experimental profile set.
    if (profileFields.profileId != kWirehairV2CertifiedProfileId)
    {
        return OuterFecResult<ValidatedDescriptorProfile>::Failure(
            OuterFecErrorCode::UnsupportedProfile,
            profileFields.profileId);
    }
    if (profileFields.messageBytes != segmentDescriptor.encodedSize)
    {
        return OuterFecResult<ValidatedDescriptorProfile>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            profileFields.messageBytes);
    }
    if (profileFields.blockBytes != segmentDescriptor.outerBlockBytes)
    {
        return OuterFecResult<ValidatedDescriptorProfile>::Failure(
            OuterFecErrorCode::OuterBlockBytesMismatch,
            profileFields.blockBytes);
    }

    const auto blockCountResult = ValidateDimensions(
        profileFields.messageBytes, profileFields.blockBytes);
    if (!blockCountResult)
    {
        return FailureFrom<ValidatedDescriptorProfile>(
            blockCountResult.Error());
    }

    // Keep the dependency parser and PixelBridge's independent canonical
    // validator in agreement without replacing the exact serialized bytes.
    const pbprotocol::ProtocolStatus protocolStatus =
        pbprotocol::ValidateWirehairV2SerializedProfile(
            segmentDescriptor.wirehairV2SerializedProfile.value(),
            segmentDescriptor.encodedSize,
            segmentDescriptor.outerBlockBytes);
    if (!protocolStatus)
    {
        return OuterFecResult<ValidatedDescriptorProfile>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            protocolStatus.Error().offset);
    }

    return OuterFecResult<ValidatedDescriptorProfile>::Success(
        ValidatedDescriptorProfile{
            &backend,
            profileFields,
            blockCountResult.Value()});
}

class CodecGuard
{
public:
    CodecGuard(
        const WirehairV2Backend& backend,
        void* const codecHandle) noexcept
        : backend_(&backend)
        , codecHandle_(codecHandle)
    {
    }

    CodecGuard(const CodecGuard&) = delete;
    CodecGuard& operator=(const CodecGuard&) = delete;

    ~CodecGuard()
    {
        if (codecHandle_ != nullptr)
        {
            backend_->freeCodec(codecHandle_);
        }
    }

    [[nodiscard]] void* Release() noexcept
    {
        void* const codecHandle = codecHandle_;
        codecHandle_ = nullptr;
        return codecHandle;
    }

private:
    const WirehairV2Backend* backend_ = nullptr;
    void* codecHandle_ = nullptr;
};

[[nodiscard]] std::uint32_t GetRequiredPayloadBytes(
    const std::uint64_t encodedSize,
    const std::uint32_t outerBlockBytes,
    const std::uint32_t blockCount,
    const std::uint32_t outerBlockId) noexcept
{
    if (outerBlockId != blockCount - 1U)
    {
        return outerBlockBytes;
    }

    const std::uint32_t remainder = static_cast<std::uint32_t>(
        encodedSize % outerBlockBytes);
    return remainder == 0 ? outerBlockBytes : remainder;
}

[[nodiscard]] std::uint64_t CreateFailureDetail(
    const int result,
    const std::uint64_t profileId,
    const std::uint32_t outerBlockBytes,
    const std::uint32_t serializedProfileBytes) noexcept
{
    switch (result)
    {
    case detail::kWirehairV2BufferTooSmall:
        return serializedProfileBytes;
    case detail::kWirehairV2UnsupportedProfile:
        return profileId;
    case detail::kWirehairV2InvalidDimensions:
        return outerBlockBytes;
    default:
        return 0;
    }
}

[[nodiscard]] OuterFecStatus ValidateCanonicalSelectionForRecreation(
    const std::span<const std::byte> exactEncodedSegment,
    const ValidatedDescriptorProfile& validated,
    const pbprotocol::WirehairV2SerializedProfile& savedProfile)
{
    pbprotocol::WirehairV2SerializedProfile selectedProfile{};
    std::uint32_t selectedProfileBytes = 0;
    void* selectedCodecHandle = nullptr;
    const int selectionResult = validated.backend->encoderCreateProfileId(
        validated.profileFields.profileId,
        exactEncodedSegment.data(),
        validated.profileFields.messageBytes,
        validated.profileFields.blockBytes,
        selectedProfile.bytes.data(),
        static_cast<std::uint32_t>(selectedProfile.bytes.size()),
        &selectedProfileBytes,
        &selectedCodecHandle);
    CodecGuard selectedCodecGuard(*validated.backend, selectedCodecHandle);
    if (selectionResult != detail::kWirehairV2Success)
    {
        const OuterFecError error = MapWirehairFailure(
            selectionResult,
            CreateFailureDetail(
                selectionResult,
                validated.profileFields.profileId,
                validated.profileFields.blockBytes,
                selectedProfileBytes));
        return OuterFecStatus::Failure(error.code, error.detail);
    }
    if (selectedCodecHandle == nullptr)
    {
        return OuterFecStatus::Failure(OuterFecErrorCode::CodecError);
    }
    if (selectedProfileBytes != selectedProfile.bytes.size())
    {
        return OuterFecStatus::Failure(
            OuterFecErrorCode::InvalidSize,
            selectedProfileBytes);
    }
    if (selectedProfile != savedProfile)
    {
        // The actual recreation below never substitutes this freshly selected
        // record. A mismatch proves the caller did not supply the exact record
        // originally emitted by Create() for these exact bytes.
        return OuterFecStatus::Failure(
            OuterFecErrorCode::InvalidDescriptor);
    }
    return OuterFecStatus::Success();
}

} // namespace

namespace detail {

struct WirehairV2DecoderImplementation
{
    void* codecHandle = nullptr;
    const WirehairV2Backend* backend = nullptr;
    std::uint64_t encodedSize = 0;
    std::uint64_t maximumAcceptedBlockIds = 0;
    std::uint32_t outerBlockBytes = 0;
    std::uint32_t blockCount = 0;
    bool ready = false;
    std::optional<OuterFecError> terminalError;
    std::unordered_map<
        std::uint32_t,
        std::array<std::byte, pbprotocol::kDigestBytes>> acceptedBlockDigests;
};

} // namespace detail

WirehairV2Encoder::WirehairV2Encoder(WirehairV2Encoder&& other) noexcept
    : codecHandle_(other.codecHandle_)
    , backend_(other.backend_)
    , serializedProfile_(other.serializedProfile_)
    , encodedSize_(other.encodedSize_)
    , outerBlockBytes_(other.outerBlockBytes_)
    , blockCount_(other.blockCount_)
    , terminalError_(other.terminalError_)
{
    other.codecHandle_ = nullptr;
    other.backend_ = nullptr;
    other.serializedProfile_ = {};
    other.encodedSize_ = 0;
    other.outerBlockBytes_ = 0;
    other.blockCount_ = 0;
    other.terminalError_.reset();
    other.moved_ = true;
}

WirehairV2Encoder& WirehairV2Encoder::operator=(
    WirehairV2Encoder&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Release();
    codecHandle_ = other.codecHandle_;
    backend_ = other.backend_;
    serializedProfile_ = other.serializedProfile_;
    encodedSize_ = other.encodedSize_;
    outerBlockBytes_ = other.outerBlockBytes_;
    blockCount_ = other.blockCount_;
    moved_ = false;
    terminalError_ = other.terminalError_;

    other.codecHandle_ = nullptr;
    other.backend_ = nullptr;
    other.serializedProfile_ = {};
    other.encodedSize_ = 0;
    other.outerBlockBytes_ = 0;
    other.blockCount_ = 0;
    other.terminalError_.reset();
    other.moved_ = true;
    return *this;
}

WirehairV2Encoder::~WirehairV2Encoder()
{
    Release();
}

void WirehairV2Encoder::Release() noexcept
{
    if (codecHandle_ != nullptr && backend_ != nullptr)
    {
        const auto* const backend =
            static_cast<const WirehairV2Backend*>(backend_);
        backend->freeCodec(codecHandle_);
    }
    codecHandle_ = nullptr;
    backend_ = nullptr;
}

OuterFecResult<WirehairV2Encoder> WirehairV2Encoder::Create(
    const std::span<const std::byte> encodedSegment,
    const std::uint32_t outerBlockBytes,
    const std::uint64_t profileId)
{
    if (!SizeFitsUint64(encodedSegment.size()))
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::UnsupportedPlatform,
            std::numeric_limits<std::uint64_t>::max());
    }
    const std::uint64_t encodedSize = SizeToUint64(encodedSegment.size());
    const auto blockCountResult = ValidateDimensions(
        encodedSize, outerBlockBytes);
    if (!blockCountResult)
    {
        return FailureFrom<WirehairV2Encoder>(blockCountResult.Error());
    }

    const WirehairV2Backend& backend = detail::GetWirehairV2Backend();
    if (!IsBackendComplete(backend))
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::CodecError);
    }

    pbprotocol::WirehairV2SerializedProfile serializedProfile{};
    std::uint32_t serializedProfileBytes = 0;
    void* codecHandle = nullptr;
    const int createResult = backend.encoderCreateProfileId(
        profileId,
        encodedSegment.data(),
        encodedSize,
        outerBlockBytes,
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()),
        &serializedProfileBytes,
        &codecHandle);
    CodecGuard codecGuard(backend, codecHandle);
    if (createResult != detail::kWirehairV2Success)
    {
        return FailureFrom<WirehairV2Encoder>(MapWirehairFailure(
            createResult,
            CreateFailureDetail(
                createResult,
                profileId,
                outerBlockBytes,
                serializedProfileBytes)));
    }
    if (codecHandle == nullptr)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::CodecError);
    }
    if (serializedProfileBytes != serializedProfile.bytes.size())
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::InvalidSize,
            serializedProfileBytes);
    }

    const auto profileResult = ValidateCanonicalProfile(
        serializedProfile, backend);
    if (!profileResult)
    {
        return FailureFrom<WirehairV2Encoder>(profileResult.Error());
    }
    const WirehairV2ProfileFields& profileFields = profileResult.Value();
    if (profileFields.profileId != profileId)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            profileFields.profileId);
    }
    if (profileFields.messageBytes != encodedSize)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            profileFields.messageBytes);
    }
    if (profileFields.blockBytes != outerBlockBytes)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::OuterBlockBytesMismatch,
            profileFields.blockBytes);
    }

    const auto serializedBlockCountResult = ValidateDimensions(
        profileFields.messageBytes, profileFields.blockBytes);
    if (!serializedBlockCountResult)
    {
        return FailureFrom<WirehairV2Encoder>(
            serializedBlockCountResult.Error());
    }
    if (serializedBlockCountResult.Value() != blockCountResult.Value())
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            serializedBlockCountResult.Value());
    }

    if (profileId == kWirehairV2CertifiedProfileId)
    {
        const pbprotocol::ProtocolStatus protocolStatus =
            pbprotocol::ValidateWirehairV2SerializedProfile(
                serializedProfile, encodedSize, outerBlockBytes);
        if (!protocolStatus)
        {
            return OuterFecResult<WirehairV2Encoder>::Failure(
                OuterFecErrorCode::InvalidDescriptor,
                protocolStatus.Error().offset);
        }
    }

    WirehairV2Encoder encoder;
    encoder.codecHandle_ = codecGuard.Release();
    encoder.backend_ = &backend;
    encoder.serializedProfile_ = serializedProfile;
    encoder.encodedSize_ = encodedSize;
    encoder.outerBlockBytes_ = outerBlockBytes;
    encoder.blockCount_ = blockCountResult.Value();
    return OuterFecResult<WirehairV2Encoder>::Success(std::move(encoder));
}

OuterFecResult<WirehairV2Encoder> WirehairV2Encoder::Recreate(
    const std::span<const std::byte> exactEncodedSegment,
    const pbprotocol::SegmentDescriptor& segmentDescriptor)
{
    const auto validatedResult = ValidateDescriptorProfile(segmentDescriptor);
    if (!validatedResult)
    {
        return FailureFrom<WirehairV2Encoder>(validatedResult.Error());
    }
    const ValidatedDescriptorProfile& validated = validatedResult.Value();

    if (!SizeFitsUint64(exactEncodedSegment.size()))
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            std::numeric_limits<std::uint64_t>::max());
    }

    const std::uint64_t actualEncodedSize =
        SizeToUint64(exactEncodedSegment.size());
    if (actualEncodedSize != segmentDescriptor.encodedSize)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            actualEncodedSize);
    }

    const pbprotocol::EncodedDigest actualDigest{
        pbprotocol::ComputeBlake3Digest(exactEncodedSegment)};
    if (actualDigest != segmentDescriptor.encodedDigest)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::EncodedDigestMismatch);
    }

    const auto& serializedProfile =
        segmentDescriptor.wirehairV2SerializedProfile.value();
    const OuterFecStatus canonicalSelectionStatus =
        ValidateCanonicalSelectionForRecreation(
            exactEncodedSegment, validated, serializedProfile);
    if (!canonicalSelectionStatus)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            canonicalSelectionStatus.Error().code,
            canonicalSelectionStatus.Error().detail);
    }

    void* codecHandle = nullptr;
    const int createResult = validated.backend->encoderCreateProfile(
        exactEncodedSegment.data(),
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()),
        &codecHandle);
    CodecGuard codecGuard(*validated.backend, codecHandle);
    if (createResult != detail::kWirehairV2Success)
    {
        return FailureFrom<WirehairV2Encoder>(MapWirehairFailure(
            createResult,
            CreateFailureDetail(
                createResult,
                validated.profileFields.profileId,
                validated.profileFields.blockBytes,
                static_cast<std::uint32_t>(serializedProfile.bytes.size()))));
    }
    if (codecHandle == nullptr)
    {
        return OuterFecResult<WirehairV2Encoder>::Failure(
            OuterFecErrorCode::CodecError);
    }

    WirehairV2Encoder encoder;
    encoder.codecHandle_ = codecGuard.Release();
    encoder.backend_ = validated.backend;
    encoder.serializedProfile_ = serializedProfile;
    encoder.encodedSize_ = segmentDescriptor.encodedSize;
    encoder.outerBlockBytes_ = segmentDescriptor.outerBlockBytes;
    encoder.blockCount_ = validated.blockCount;
    return OuterFecResult<WirehairV2Encoder>::Success(std::move(encoder));
}

OuterFecResult<std::uint32_t> WirehairV2Encoder::EncodeBlock(
    const std::uint32_t outerBlockId,
    const std::span<std::byte> output)
{
    if (moved_ || codecHandle_ == nullptr || backend_ == nullptr)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (terminalError_.has_value())
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidState,
            static_cast<std::uint64_t>(terminalError_->code));
    }

    const std::uint32_t requiredBytes = GetRequiredPayloadBytes(
        encodedSize_, outerBlockBytes_, blockCount_, outerBlockId);
    if (output.size() < requiredBytes)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::BufferTooSmall,
            requiredBytes);
    }

    const auto* const backend =
        static_cast<const WirehairV2Backend*>(backend_);
    std::uint32_t writtenBytes = 0;
    const int encodeResult = backend->encode(
        codecHandle_,
        outerBlockId,
        output.data(),
        requiredBytes,
        &writtenBytes);
    if (encodeResult != detail::kWirehairV2Success)
    {
        const std::uint64_t resultDetail =
            encodeResult == detail::kWirehairV2BufferTooSmall
            ? (writtenBytes == 0 ? requiredBytes : writtenBytes)
            : 0;
        const OuterFecError error =
            MapWirehairFailure(encodeResult, resultDetail);
        if (error.code != OuterFecErrorCode::BufferTooSmall)
        {
            terminalError_ = error;
        }
        return FailureFrom<std::uint32_t>(error);
    }
    if (writtenBytes != requiredBytes)
    {
        const OuterFecError error{
            OuterFecErrorCode::CodecError,
            writtenBytes};
        terminalError_ = error;
        return FailureFrom<std::uint32_t>(error);
    }

    return OuterFecResult<std::uint32_t>::Success(writtenBytes);
}

pbprotocol::WirehairV2SerializedProfile
WirehairV2Encoder::GetSerializedProfile() const noexcept
{
    return serializedProfile_;
}

std::uint32_t WirehairV2Encoder::GetBlockCount() const noexcept
{
    return blockCount_;
}

std::uint64_t WirehairV2Encoder::GetEncodedSize() const noexcept
{
    return encodedSize_;
}

std::uint32_t WirehairV2Encoder::GetOuterBlockBytes() const noexcept
{
    return outerBlockBytes_;
}

WirehairV2Decoder::WirehairV2Decoder(WirehairV2Decoder&& other) noexcept
    : implementation_(std::move(other.implementation_))
{
}

WirehairV2Decoder& WirehairV2Decoder::operator=(
    WirehairV2Decoder&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    Release();
    implementation_ = std::move(other.implementation_);
    return *this;
}

WirehairV2Decoder::~WirehairV2Decoder()
{
    Release();
}

void WirehairV2Decoder::Release() noexcept
{
    if (implementation_ != nullptr
        && implementation_->codecHandle != nullptr
        && implementation_->backend != nullptr)
    {
        implementation_->backend->freeCodec(
            implementation_->codecHandle);
        implementation_->codecHandle = nullptr;
    }
    implementation_.reset();
}

OuterFecResult<WirehairV2Decoder> WirehairV2Decoder::Create(
    const pbprotocol::SegmentDescriptor& segmentDescriptor)
{
    const auto validatedResult = ValidateDescriptorProfile(segmentDescriptor);
    if (!validatedResult)
    {
        return FailureFrom<WirehairV2Decoder>(validatedResult.Error());
    }
    const ValidatedDescriptorProfile& validated = validatedResult.Value();

    std::unique_ptr<detail::WirehairV2DecoderImplementation> implementation;
    try
    {
        implementation =
            std::make_unique<detail::WirehairV2DecoderImplementation>();
        implementation->maximumAcceptedBlockIds =
            static_cast<std::uint64_t>(validated.blockCount)
            + kWirehairV2RetainedExtraIds;
        implementation->acceptedBlockDigests.reserve(
            static_cast<std::size_t>(
                implementation->maximumAcceptedBlockIds));
    }
    catch (const std::bad_alloc&)
    {
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::OutOfMemory);
    }
    catch (const std::length_error&)
    {
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::OutOfMemory,
            validated.blockCount);
    }

    const auto& serializedProfile =
        segmentDescriptor.wirehairV2SerializedProfile.value();
    void* codecHandle = nullptr;
    const int createResult = validated.backend->decoderCreate(
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()),
        &codecHandle);
    CodecGuard codecGuard(*validated.backend, codecHandle);
    if (createResult != detail::kWirehairV2Success)
    {
        return FailureFrom<WirehairV2Decoder>(
            MapWirehairFailure(createResult));
    }
    if (codecHandle == nullptr)
    {
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::CodecError);
    }

    implementation->codecHandle = codecGuard.Release();
    implementation->backend = validated.backend;
    implementation->encodedSize = segmentDescriptor.encodedSize;
    implementation->outerBlockBytes = segmentDescriptor.outerBlockBytes;
    implementation->blockCount = validated.blockCount;

    WirehairV2Decoder decoder;
    decoder.implementation_ = std::move(implementation);
    return OuterFecResult<WirehairV2Decoder>::Success(std::move(decoder));
}

OuterFecResult<DecodeDisposition> WirehairV2Decoder::DecodeBlock(
    const std::uint32_t outerBlockId,
    const std::span<const std::byte> payload)
{
    if (implementation_ == nullptr
        || implementation_->codecHandle == nullptr
        || implementation_->backend == nullptr)
    {
        return OuterFecResult<DecodeDisposition>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (implementation_->terminalError.has_value())
    {
        return OuterFecResult<DecodeDisposition>::Failure(
            OuterFecErrorCode::InvalidState,
            static_cast<std::uint64_t>(
                implementation_->terminalError->code));
    }

    const std::uint32_t requiredBytes = GetRequiredPayloadBytes(
        implementation_->encodedSize,
        implementation_->outerBlockBytes,
        implementation_->blockCount,
        outerBlockId);
    if (payload.size() != requiredBytes)
    {
        const OuterFecError error{
            OuterFecErrorCode::InvalidInput,
            SizeToUint64(payload.size())};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }

    const auto payloadDigest = pbprotocol::ComputeBlake3Digest(payload);
    const auto existingBlock =
        implementation_->acceptedBlockDigests.find(outerBlockId);
    if (existingBlock != implementation_->acceptedBlockDigests.end())
    {
        if (existingBlock->second != payloadDigest)
        {
            const OuterFecError error{
                OuterFecErrorCode::InvalidInput,
                outerBlockId};
            implementation_->terminalError = error;
            return FailureFrom<DecodeDisposition>(error);
        }

        return OuterFecResult<DecodeDisposition>::Success(
            implementation_->ready
                ? DecodeDisposition::Ready
                : DecodeDisposition::NeedMore);
    }

    if (implementation_->ready)
    {
        return OuterFecResult<DecodeDisposition>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (implementation_->acceptedBlockDigests.size()
        >= implementation_->maximumAcceptedBlockIds)
    {
        const OuterFecError error{
            OuterFecErrorCode::ExtraInsufficient,
            implementation_->maximumAcceptedBlockIds};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }

    try
    {
        const auto insertResult =
            implementation_->acceptedBlockDigests.try_emplace(
                outerBlockId, payloadDigest);
        if (!insertResult.second)
        {
            const OuterFecError error{
                OuterFecErrorCode::CodecError,
                outerBlockId};
            implementation_->terminalError = error;
            return FailureFrom<DecodeDisposition>(error);
        }
    }
    catch (const std::bad_alloc&)
    {
        const OuterFecError error{OuterFecErrorCode::OutOfMemory, 0};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }
    catch (const std::length_error&)
    {
        const OuterFecError error{
            OuterFecErrorCode::OutOfMemory,
            implementation_->acceptedBlockDigests.size()};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }

    const int decodeResult = implementation_->backend->decode(
        implementation_->codecHandle,
        outerBlockId,
        payload.data(),
        requiredBytes);
    if (decodeResult == detail::kWirehairV2NeedMore)
    {
        return OuterFecResult<DecodeDisposition>::Success(
            DecodeDisposition::NeedMore);
    }
    if (decodeResult == detail::kWirehairV2Success)
    {
        implementation_->ready = true;
        return OuterFecResult<DecodeDisposition>::Success(
            DecodeDisposition::Ready);
    }

    implementation_->acceptedBlockDigests.erase(outerBlockId);
    const std::uint64_t resultDetail =
        decodeResult == detail::kWirehairV2BufferTooSmall
        ? requiredBytes
        : 0;
    const OuterFecError error =
        MapWirehairFailure(decodeResult, resultDetail);
    implementation_->terminalError = error;
    return FailureFrom<DecodeDisposition>(error);
}

OuterFecResult<std::uint64_t> WirehairV2Decoder::Recover(
    const std::span<std::byte> output)
{
    if (implementation_ == nullptr
        || implementation_->codecHandle == nullptr
        || implementation_->backend == nullptr
        || !implementation_->ready
        || implementation_->terminalError.has_value())
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::InvalidState);
    }

    if (SizeToUint64(output.size()) < implementation_->encodedSize)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::BufferTooSmall,
            implementation_->encodedSize);
    }

    std::uint64_t recoveredBytes = 0;
    const int recoverResult = implementation_->backend->recover(
        implementation_->codecHandle,
        output.data(),
        implementation_->encodedSize,
        &recoveredBytes);
    if (recoverResult == detail::kWirehairV2BufferTooSmall)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::BufferTooSmall,
            recoveredBytes == 0
                ? implementation_->encodedSize
                : recoveredBytes);
    }
    if (recoverResult != detail::kWirehairV2Success)
    {
        const OuterFecError error =
            MapWirehairFailure(recoverResult);
        implementation_->terminalError = error;
        return FailureFrom<std::uint64_t>(error);
    }
    if (recoveredBytes != implementation_->encodedSize)
    {
        const OuterFecError error{
            OuterFecErrorCode::CodecError,
            recoveredBytes};
        implementation_->terminalError = error;
        return FailureFrom<std::uint64_t>(error);
    }

    return OuterFecResult<std::uint64_t>::Success(recoveredBytes);
}

} // namespace pbouterfec
