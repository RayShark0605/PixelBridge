#include "pbouterfec/wirehair_v2.h"

#include "outer_fec_decoder_resource_internal.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "wirehair_v2_backend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pbouterfec {
namespace {

using detail::WirehairV2Backend;
using detail::WirehairV2ProfileFields;

inline constexpr std::uint64_t kWirehairV2RetainedExtraIds = 1024;
inline constexpr std::uint64_t kWirehairV2InitialExtraRows = 32;
inline constexpr std::size_t kAcceptedBlockFingerprintBytes = 16;
inline constexpr std::size_t kMaximumAcceptedBlockProbeCount = 64;
inline constexpr std::uint64_t kDecoderFixedAdmissionBytes = 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDecoderEncodedWorkMultiplier = 4;
inline constexpr std::uint64_t kDecoderRowAdmissionBytes = 256;
inline constexpr std::uint64_t kRetainedPayloadPeakMultiplier = 2;
// The pinned 067ca7c Wirehair ReceivedPacketRecord has an upstream
// static_assert(sizeof(...) == 24). Re-audit this charge with any dependency
// revision; undercounting the private accepted-ID table defeats admission.
inline constexpr std::uint64_t kWirehairSlotPeakAdmissionBytes = 24;
inline constexpr std::uint64_t kWrapperSlotPeakAdmissionBytes = 48;

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
    if (blockBytes == 0 ||
        blockBytes > pbprotocol::kMaximumTransportPayloadBytes)
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

struct DecoderResourceEstimate
{
    std::uint64_t maximumAcceptedBlockIds = 0;
    std::uint64_t initialAcceptedBlockIds = 0;
    std::uint64_t maximumTableCapacity = 0;
    std::uint64_t reservationBytes = 0;
};

[[nodiscard]] OuterFecResult<std::uint64_t> GetTableCapacity(
    const std::uint64_t entryCount)
{
    const auto doubledResult = pbprotocol::CheckedMultiplyUint64(
        entryCount, 2ULL);
    if (!doubledResult)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }

    const std::uint64_t requiredCapacity = doubledResult.Value();
    std::uint64_t capacity = 1;
    while (capacity < requiredCapacity)
    {
        const auto nextCapacityResult = pbprotocol::CheckedMultiplyUint64(
            capacity, 2ULL);
        if (!nextCapacityResult)
        {
            return OuterFecResult<std::uint64_t>::Failure(
                OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
                std::numeric_limits<std::uint64_t>::max());
        }
        capacity = nextCapacityResult.Value();
    }
    return OuterFecResult<std::uint64_t>::Success(capacity);
}

[[nodiscard]] bool AddReservationTerm(
    const std::uint64_t term,
    std::uint64_t& reservationBytes) noexcept
{
    const auto sumResult = pbprotocol::CheckedAddUint64(
        reservationBytes, term);
    if (!sumResult)
    {
        return false;
    }
    reservationBytes = sumResult.Value();
    return true;
}

[[nodiscard]] OuterFecResult<DecoderResourceEstimate>
CalculateDecoderResourceEstimate(
    const WirehairV2ProfileFields& profileFields,
    const std::uint32_t blockCount)
{
    const auto maximumAcceptedResult = pbprotocol::CheckedAddUint64(
        static_cast<std::uint64_t>(blockCount),
        kWirehairV2RetainedExtraIds);
    const auto initialAcceptedResult = pbprotocol::CheckedAddUint64(
        static_cast<std::uint64_t>(blockCount),
        kWirehairV2InitialExtraRows);
    if (!maximumAcceptedResult || !initialAcceptedResult)
    {
        return OuterFecResult<DecoderResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }

    const std::uint64_t maximumAcceptedBlockIds =
        maximumAcceptedResult.Value();
    const auto maximumCapacityResult = GetTableCapacity(
        maximumAcceptedBlockIds);
    if (!maximumCapacityResult)
    {
        return FailureFrom<DecoderResourceEstimate>(
            maximumCapacityResult.Error());
    }

    const auto retainedPayloadBytesResult = pbprotocol::CheckedMultiplyUint64(
        maximumAcceptedBlockIds,
        static_cast<std::uint64_t>(profileFields.blockBytes));
    if (!retainedPayloadBytesResult)
    {
        return OuterFecResult<DecoderResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }
    const auto retainedPayloadPeakResult = pbprotocol::CheckedMultiplyUint64(
        retainedPayloadBytesResult.Value(),
        kRetainedPayloadPeakMultiplier);
    const auto encodedWorkResult = pbprotocol::CheckedMultiplyUint64(
        profileFields.messageBytes,
        kDecoderEncodedWorkMultiplier);
    const auto rowStateResult = pbprotocol::CheckedMultiplyUint64(
        maximumAcceptedBlockIds,
        kDecoderRowAdmissionBytes);
    const auto tableBytesPerSlotResult = pbprotocol::CheckedAddUint64(
        kWirehairSlotPeakAdmissionBytes,
        kWrapperSlotPeakAdmissionBytes);
    if (!retainedPayloadPeakResult || !encodedWorkResult || !rowStateResult ||
        !tableBytesPerSlotResult)
    {
        return OuterFecResult<DecoderResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }
    const auto tableStateResult = pbprotocol::CheckedMultiplyUint64(
        maximumCapacityResult.Value(),
        tableBytesPerSlotResult.Value());
    if (!tableStateResult)
    {
        return OuterFecResult<DecoderResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }

    // This is a conservative admission charge rather than allocator telemetry:
    // it covers peak vector growth for the full accepted-payload window, peak
    // growth of both bounded ID tables, a four-message solver/workspace
    // allowance, per-row state, and fixed state.
    std::uint64_t reservationBytes = 0;
    if (!AddReservationTerm(
            retainedPayloadPeakResult.Value(), reservationBytes) ||
        !AddReservationTerm(encodedWorkResult.Value(), reservationBytes) ||
        !AddReservationTerm(rowStateResult.Value(), reservationBytes) ||
        !AddReservationTerm(tableStateResult.Value(), reservationBytes) ||
        !AddReservationTerm(kDecoderFixedAdmissionBytes, reservationBytes))
    {
        return OuterFecResult<DecoderResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }

    return OuterFecResult<DecoderResourceEstimate>::Success(
        DecoderResourceEstimate{
            maximumAcceptedBlockIds,
            initialAcceptedResult.Value(),
            maximumCapacityResult.Value(),
            reservationBytes});
}

using AcceptedBlockFingerprint =
    std::array<std::byte, kAcceptedBlockFingerprintBytes>;

enum class AcceptedBlockInsertDisposition : std::uint8_t
{
    Inserted,
    Duplicate,
    Conflict,
    NotFound,
    Full,
    ProbeLimitExceeded
};

struct AcceptedBlockSlot
{
    std::uint32_t outerBlockId = 0;
    AcceptedBlockFingerprint fingerprint{};
    bool occupied = false;
};

static_assert(sizeof(AcceptedBlockSlot) <= 32);

enum class AcceptedBlockProbeDisposition : std::uint8_t
{
    Found,
    Empty,
    ProbeLimitExceeded
};

struct AcceptedBlockProbeResult
{
    AcceptedBlockProbeDisposition disposition =
        AcceptedBlockProbeDisposition::ProbeLimitExceeded;
    std::size_t slotIndex = 0;
};

class AcceptedBlockFingerprintTable
{
public:
    void Initialize(
        const std::uint64_t initialEntryCount,
        const std::uint64_t maximumEntryCount,
        const std::uint64_t maximumCapacity,
        const std::uint64_t hashSalt)
    {
        const auto initialCapacityResult = GetTableCapacity(
            initialEntryCount);
        if (!initialCapacityResult ||
            initialCapacityResult.Value() > maximumCapacity)
        {
            throw std::length_error(
                "accepted block table capacity is invalid");
        }

        const auto initialCapacitySizeResult =
            pbprotocol::CheckedUint64ToSize(initialCapacityResult.Value());
        const auto maximumEntrySizeResult =
            pbprotocol::CheckedUint64ToSize(maximumEntryCount);
        const auto maximumCapacitySizeResult =
            pbprotocol::CheckedUint64ToSize(maximumCapacity);
        if (!initialCapacitySizeResult || !maximumEntrySizeResult ||
            !maximumCapacitySizeResult)
        {
            throw std::length_error(
                "accepted block table does not fit size_t");
        }

        std::vector<AcceptedBlockSlot> initialSlots(
            initialCapacitySizeResult.Value());
        slots_.swap(initialSlots);
        maximumEntryCount_ = maximumEntrySizeResult.Value();
        maximumCapacity_ = maximumCapacitySizeResult.Value();
        entryCount_ = 0;
        hashSalt_ = hashSalt;
    }

    [[nodiscard]] AcceptedBlockInsertDisposition CheckAndInsert(
        const std::uint32_t outerBlockId,
        const AcceptedBlockFingerprint& fingerprint,
        const bool insertIfAbsent)
    {
        AcceptedBlockProbeResult probeResult = Probe(
            slots_, outerBlockId, hashSalt_);
        if (probeResult.disposition == AcceptedBlockProbeDisposition::Found)
        {
            return slots_[probeResult.slotIndex].fingerprint == fingerprint
                ? AcceptedBlockInsertDisposition::Duplicate
                : AcceptedBlockInsertDisposition::Conflict;
        }
        if (probeResult.disposition ==
            AcceptedBlockProbeDisposition::ProbeLimitExceeded)
        {
            return AcceptedBlockInsertDisposition::ProbeLimitExceeded;
        }
        if (!insertIfAbsent)
        {
            return AcceptedBlockInsertDisposition::NotFound;
        }
        if (entryCount_ >= maximumEntryCount_)
        {
            return AcceptedBlockInsertDisposition::Full;
        }

        if (entryCount_ >= slots_.size() / 2U)
        {
            const AcceptedBlockInsertDisposition growResult = Grow();
            if (growResult != AcceptedBlockInsertDisposition::Inserted)
            {
                return growResult;
            }
            probeResult = Probe(slots_, outerBlockId, hashSalt_);
            if (probeResult.disposition ==
                AcceptedBlockProbeDisposition::ProbeLimitExceeded)
            {
                return AcceptedBlockInsertDisposition::ProbeLimitExceeded;
            }
            if (probeResult.disposition !=
                AcceptedBlockProbeDisposition::Empty)
            {
                return AcceptedBlockInsertDisposition::Conflict;
            }
        }

        AcceptedBlockSlot& slot = slots_[probeResult.slotIndex];
        slot.outerBlockId = outerBlockId;
        slot.fingerprint = fingerprint;
        slot.occupied = true;
        entryCount_++;
        return AcceptedBlockInsertDisposition::Inserted;
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return entryCount_;
    }

    [[nodiscard]] std::size_t MaximumEntryCount() const noexcept
    {
        return maximumEntryCount_;
    }

private:
    [[nodiscard]] static AcceptedBlockProbeResult Probe(
        const std::vector<AcceptedBlockSlot>& slots,
        const std::uint32_t outerBlockId,
        const std::uint64_t hashSalt) noexcept
    {
        if (slots.empty())
        {
            return {};
        }

        const std::size_t mask = slots.size() - 1U;
        std::size_t slotIndex =
            static_cast<std::size_t>(
                detail::HashWirehairV2AcceptedBlockId(
                    outerBlockId, hashSalt)) & mask;
        const std::size_t probeCount = std::min(
            slots.size(), kMaximumAcceptedBlockProbeCount);
        for (std::size_t probeIndex = 0;
            probeIndex < probeCount;
            probeIndex++)
        {
            const AcceptedBlockSlot& slot = slots[slotIndex];
            if (!slot.occupied)
            {
                return AcceptedBlockProbeResult{
                    AcceptedBlockProbeDisposition::Empty,
                    slotIndex};
            }
            if (slot.outerBlockId == outerBlockId)
            {
                return AcceptedBlockProbeResult{
                    AcceptedBlockProbeDisposition::Found,
                    slotIndex};
            }
            slotIndex = (slotIndex + 1U) & mask;
        }
        return {};
    }

    [[nodiscard]] static bool InsertExisting(
        std::vector<AcceptedBlockSlot>& slots,
        const AcceptedBlockSlot& existingSlot,
        const std::uint64_t hashSalt) noexcept
    {
        const AcceptedBlockProbeResult probeResult = Probe(
            slots, existingSlot.outerBlockId, hashSalt);
        if (probeResult.disposition != AcceptedBlockProbeDisposition::Empty)
        {
            return false;
        }
        slots[probeResult.slotIndex] = existingSlot;
        return true;
    }

    [[nodiscard]] AcceptedBlockInsertDisposition Grow()
    {
        if (slots_.size() >= maximumCapacity_ ||
            slots_.size() > std::numeric_limits<std::size_t>::max() / 2U)
        {
            return AcceptedBlockInsertDisposition::Full;
        }

        const std::size_t newCapacity = std::min(
            slots_.size() * 2U, maximumCapacity_);
        std::vector<AcceptedBlockSlot> newSlots(newCapacity);
        for (const AcceptedBlockSlot& slot : slots_)
        {
            if (slot.occupied &&
                !InsertExisting(newSlots, slot, hashSalt_))
            {
                return AcceptedBlockInsertDisposition::ProbeLimitExceeded;
            }
        }
        slots_.swap(newSlots);
        return AcceptedBlockInsertDisposition::Inserted;
    }

    std::vector<AcceptedBlockSlot> slots_;
    std::size_t entryCount_ = 0;
    std::size_t maximumEntryCount_ = 0;
    std::size_t maximumCapacity_ = 0;
    std::uint64_t hashSalt_ = 0;
};

[[nodiscard]] AcceptedBlockFingerprint ComputeAcceptedBlockFingerprint(
    const std::span<const std::byte> payload) noexcept
{
    const std::array<std::byte, pbprotocol::kDigestBytes> digest =
        pbprotocol::ComputeBlake3Digest(payload);
    AcceptedBlockFingerprint fingerprint{};
    std::copy_n(
        digest.begin(), fingerprint.size(), fingerprint.begin());
    return fingerprint;
}

} // namespace

namespace detail {

struct WirehairV2DecoderImplementation
{
    void* codecHandle = nullptr;
    const WirehairV2Backend* backend = nullptr;
    pbprotocol::EncodedDigest expectedDigest{};
    std::uint64_t encodedSize = 0;
    std::uint32_t outerBlockBytes = 0;
    std::uint32_t blockCount = 0;
    bool ready = false;
    std::optional<OuterFecError> terminalError;
    OuterFecDecoderReservation resourceReservation;
    AcceptedBlockFingerprintTable acceptedBlocks;
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
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
    const std::uint32_t expectedOuterBlockBytes,
    const OuterFecDecoderResourceManager& resourceManager)
{
    if (boundSegmentDescriptor.GetDescriptor().outerBlockBytes !=
        expectedOuterBlockBytes)
    {
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::OuterBlockBytesMismatch,
            boundSegmentDescriptor.GetDescriptor().outerBlockBytes);
    }
    return CreateFromDescriptor(
        boundSegmentDescriptor.GetDescriptor(),
        resourceManager);
}

OuterFecResult<WirehairV2Decoder>
WirehairV2Decoder::CreateFromDescriptor(
    const pbprotocol::SegmentDescriptor& segmentDescriptor,
    const OuterFecDecoderResourceManager& resourceManager)
{
    if (resourceManager.state_ == nullptr)
    {
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::InvalidState);
    }

    const pbprotocol::ReceiverResourcePolicy& resourcePolicy =
        resourceManager.state_->resourcePolicy;
    if (segmentDescriptor.encodedSize >
        resourcePolicy.maxEncodedSegmentBytes)
    {
        detail::CountOuterFecDecoderQuotaExceeded(resourceManager.state_);
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            segmentDescriptor.encodedSize);
    }
    if (segmentDescriptor.outerBlockBytes >
        resourcePolicy.maxOuterBlockBytes)
    {
        detail::CountOuterFecDecoderQuotaExceeded(resourceManager.state_);
        return OuterFecResult<WirehairV2Decoder>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            segmentDescriptor.outerBlockBytes);
    }

    const auto validatedResult = ValidateDescriptorProfile(segmentDescriptor);
    if (!validatedResult)
    {
        return FailureFrom<WirehairV2Decoder>(validatedResult.Error());
    }
    const ValidatedDescriptorProfile& validated = validatedResult.Value();

    const auto estimateResult = CalculateDecoderResourceEstimate(
        validated.profileFields, validated.blockCount);
    if (!estimateResult)
    {
        // The estimator only fails with the quota code today; guard
        // explicitly so a future non-quota failure is not miscounted.
        if (estimateResult.Error().code ==
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded)
        {
            detail::CountOuterFecDecoderQuotaExceeded(
                resourceManager.state_);
        }
        return FailureFrom<WirehairV2Decoder>(estimateResult.Error());
    }
    const DecoderResourceEstimate& estimate = estimateResult.Value();

    const auto hashSaltResult =
        detail::GenerateWirehairV2AcceptedBlockHashSalt();
    if (!hashSaltResult)
    {
        return FailureFrom<WirehairV2Decoder>(hashSaltResult.Error());
    }

    auto reservationResult = detail::AcquireOuterFecDecoderReservation(
        resourceManager.state_, estimate.reservationBytes);
    if (!reservationResult)
    {
        return FailureFrom<WirehairV2Decoder>(reservationResult.Error());
    }
    detail::OuterFecDecoderReservation reservation =
        std::move(reservationResult).Value();

    std::unique_ptr<detail::WirehairV2DecoderImplementation> implementation;
    try
    {
        implementation =
            std::make_unique<detail::WirehairV2DecoderImplementation>();
        implementation->acceptedBlocks.Initialize(
            estimate.initialAcceptedBlockIds,
            estimate.maximumAcceptedBlockIds,
            estimate.maximumTableCapacity,
            hashSaltResult.Value());
        implementation->resourceReservation = std::move(reservation);
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
    implementation->expectedDigest = segmentDescriptor.encodedDigest;
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

    const AcceptedBlockFingerprint fingerprint =
        ComputeAcceptedBlockFingerprint(payload);
    AcceptedBlockInsertDisposition insertDisposition =
        AcceptedBlockInsertDisposition::ProbeLimitExceeded;
    try
    {
        insertDisposition = implementation_->acceptedBlocks.CheckAndInsert(
            outerBlockId,
            fingerprint,
            !implementation_->ready);
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
            implementation_->acceptedBlocks.Size()};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }

    switch (insertDisposition)
    {
    case AcceptedBlockInsertDisposition::Duplicate:
        return OuterFecResult<DecodeDisposition>::Success(
            implementation_->ready
                ? DecodeDisposition::Ready
                : DecodeDisposition::NeedMore);
    case AcceptedBlockInsertDisposition::Conflict:
    {
        const OuterFecError error{
            OuterFecErrorCode::OuterBlockConflict,
            outerBlockId};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }
    case AcceptedBlockInsertDisposition::NotFound:
        return OuterFecResult<DecodeDisposition>::Failure(
            OuterFecErrorCode::InvalidState);
    case AcceptedBlockInsertDisposition::Full:
    {
        const OuterFecError error{
            OuterFecErrorCode::ExtraInsufficient,
            implementation_->acceptedBlocks.MaximumEntryCount()};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }
    case AcceptedBlockInsertDisposition::ProbeLimitExceeded:
    {
        const OuterFecError error{
            OuterFecErrorCode::ExtraInsufficient,
            kMaximumAcceptedBlockProbeCount};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }
    case AcceptedBlockInsertDisposition::Inserted:
        break;
    default:
    {
        const OuterFecError error{
            OuterFecErrorCode::CodecError,
            outerBlockId};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    }
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

    const auto recoveredSizeResult = pbprotocol::CheckedUint64ToSize(
        recoveredBytes);
    if (!recoveredSizeResult)
    {
        const OuterFecError error{
            OuterFecErrorCode::EncodedSizeMismatch,
            recoveredBytes};
        implementation_->terminalError = error;
        return FailureFrom<std::uint64_t>(error);
    }
    const std::span<const std::byte> recoveredPayload = output.first(
        recoveredSizeResult.Value());
    const pbprotocol::EncodedDigest actualDigest{
        pbprotocol::ComputeBlake3Digest(recoveredPayload)};
    if (actualDigest != implementation_->expectedDigest)
    {
        const OuterFecError error{
            OuterFecErrorCode::EncodedDigestMismatch,
            0};
        implementation_->terminalError = error;
        return FailureFrom<std::uint64_t>(error);
    }

    return OuterFecResult<std::uint64_t>::Success(recoveredBytes);
}

} // namespace pbouterfec
