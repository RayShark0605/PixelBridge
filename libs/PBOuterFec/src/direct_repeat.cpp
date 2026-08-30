#include "pbouterfec/direct_repeat.h"

#include "outer_fec_decoder_resource_internal.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
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

namespace pbouterfec
{
namespace
{

inline constexpr std::uint64_t kReceivedBitmapWordBits = 64;
inline constexpr std::uint64_t kDirectRepeatFixedAdmissionBytes = 4096;

template <typename ValueType>
[[nodiscard]] OuterFecResult<ValueType> FailureFrom(
    const OuterFecError& error)
{
    return OuterFecResult<ValueType>::Failure(error.code, error.detail);
}

[[nodiscard]] bool SizeFitsUint64(const std::size_t byteCount) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    {
        return byteCount <= std::numeric_limits<std::uint64_t>::max();
    }
    return true;
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

[[nodiscard]] OuterFecResult<std::uint64_t> GetDirectBlockCount(
    const std::uint64_t encodedSize,
    const std::uint32_t outerBlockBytes)
{
    const auto blockCountResult = pbprotocol::GetDirectRepeatBlockCount(
        encodedSize, outerBlockBytes);
    if (!blockCountResult)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            outerBlockBytes == 0 ? outerBlockBytes : encodedSize);
    }
    return OuterFecResult<std::uint64_t>::Success(
        blockCountResult.Value());
}

struct ValidatedDirectDescriptor
{
    std::uint64_t blockCount = 0;
};

[[nodiscard]] OuterFecResult<ValidatedDirectDescriptor>
ValidateDirectDescriptor(
    const pbprotocol::SegmentDescriptor& segmentDescriptor)
{
    if (segmentDescriptor.outerFecMode !=
            pbprotocol::OuterFecMode::DirectRepeat ||
        segmentDescriptor.wirehairV2SerializedProfile.has_value())
    {
        return OuterFecResult<ValidatedDirectDescriptor>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            static_cast<std::uint64_t>(segmentDescriptor.outerFecMode));
    }
    if (segmentDescriptor.encodedSize == 0)
    {
        return OuterFecResult<ValidatedDirectDescriptor>::Failure(
            OuterFecErrorCode::InvalidDescriptor,
            segmentDescriptor.encodedSize);
    }

    const auto blockCountResult = GetDirectBlockCount(
        segmentDescriptor.encodedSize,
        segmentDescriptor.outerBlockBytes);
    if (!blockCountResult)
    {
        return FailureFrom<ValidatedDirectDescriptor>(
            blockCountResult.Error());
    }
    if (blockCountResult.Value() == 0)
    {
        return OuterFecResult<ValidatedDirectDescriptor>::Failure(
            OuterFecErrorCode::InvalidDescriptor);
    }

    return OuterFecResult<ValidatedDirectDescriptor>::Success(
        ValidatedDirectDescriptor{blockCountResult.Value()});
}

struct DirectRepeatResourceEstimate
{
    std::size_t encodedSize = 0;
    std::size_t bitmapWordCount = 0;
    std::uint64_t reservationBytes = 0;
};

[[nodiscard]] OuterFecResult<DirectRepeatResourceEstimate>
CalculateDirectRepeatResourceEstimate(
    const std::uint64_t encodedSize,
    const std::uint64_t blockCount)
{
    const auto encodedSizeResult = pbprotocol::CheckedUint64ToSize(encodedSize);
    if (!encodedSizeResult || blockCount == 0)
    {
        return OuterFecResult<DirectRepeatResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            encodedSize);
    }

    const std::uint64_t bitmapWordCount =
        1ULL + ((blockCount - 1ULL) / kReceivedBitmapWordBits);
    const auto bitmapWordSizeResult =
        pbprotocol::CheckedUint64ToSize(bitmapWordCount);
    const auto bitmapBytesResult = pbprotocol::CheckedMultiplyUint64(
        bitmapWordCount,
        static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    if (!bitmapWordSizeResult || !bitmapBytesResult)
    {
        return OuterFecResult<DirectRepeatResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            blockCount);
    }

    const std::vector<std::byte> emptyEncodedBytes;
    const std::vector<std::uint64_t> emptyBitmap;
    if (encodedSizeResult.Value() > emptyEncodedBytes.max_size() ||
        bitmapWordSizeResult.Value() > emptyBitmap.max_size())
    {
        return OuterFecResult<DirectRepeatResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            encodedSize);
    }

    const auto payloadAndBitmapResult = pbprotocol::CheckedAddUint64(
        encodedSize,
        bitmapBytesResult.Value());
    if (!payloadAndBitmapResult)
    {
        return OuterFecResult<DirectRepeatResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }
    const auto reservationResult = pbprotocol::CheckedAddUint64(
        payloadAndBitmapResult.Value(),
        kDirectRepeatFixedAdmissionBytes);
    if (!reservationResult)
    {
        return OuterFecResult<DirectRepeatResourceEstimate>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            std::numeric_limits<std::uint64_t>::max());
    }

    return OuterFecResult<DirectRepeatResourceEstimate>::Success(
        DirectRepeatResourceEstimate{
            encodedSizeResult.Value(),
            bitmapWordSizeResult.Value(),
            reservationResult.Value()});
}

[[nodiscard]] OuterFecResult<std::uint64_t> GetBlockOffset(
    const std::uint32_t outerBlockId,
    const std::uint32_t outerBlockBytes)
{
    const auto offsetResult = pbprotocol::CheckedMultiplyUint64(
        static_cast<std::uint64_t>(outerBlockId),
        static_cast<std::uint64_t>(outerBlockBytes));
    if (!offsetResult)
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            outerBlockId);
    }
    return OuterFecResult<std::uint64_t>::Success(offsetResult.Value());
}

[[nodiscard]] std::uint32_t GetRequiredPayloadBytes(
    const std::uint64_t encodedSize,
    const std::uint64_t blockOffset,
    const std::uint32_t outerBlockBytes) noexcept
{
    const std::uint64_t remainingBytes = encodedSize - blockOffset;
    return remainingBytes < outerBlockBytes
        ? static_cast<std::uint32_t>(remainingBytes)
        : outerBlockBytes;
}

} // namespace

namespace detail
{

struct DirectRepeatDecoderImplementation
{
    std::vector<std::byte> encodedBytes;
    std::vector<std::uint64_t> receivedBitmap;
    pbprotocol::EncodedDigest expectedDigest{};
    std::uint64_t encodedSize = 0;
    std::uint64_t blockCount = 0;
    std::uint64_t receivedBlockCount = 0;
    std::uint32_t outerBlockBytes = 0;
    bool ready = false;
    std::optional<OuterFecError> terminalError;
    OuterFecDecoderReservation resourceReservation;
};

} // namespace detail

OuterFecResult<pbprotocol::OuterFecMode> ChooseOuterFecMode(
    const std::uint64_t encodedSize,
    const std::uint32_t outerBlockBytes,
    const OuterFecModeSelectionPolicy& selectionPolicy)
{
    if (selectionPolicy.maximumEfficientDirectRepeatBlockCount == 0 ||
        selectionPolicy.maximumEfficientDirectRepeatBlockCount >
            kWirehairV2MaximumBlockCount)
    {
        return OuterFecResult<pbprotocol::OuterFecMode>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            selectionPolicy.maximumEfficientDirectRepeatBlockCount);
    }

    const auto blockCountResult = GetDirectBlockCount(
        encodedSize, outerBlockBytes);
    if (!blockCountResult)
    {
        return FailureFrom<pbprotocol::OuterFecMode>(
            blockCountResult.Error());
    }

    const std::uint64_t blockCount = blockCountResult.Value();
    if (blockCount < kWirehairV2MinimumBlockCount ||
        blockCount <=
            selectionPolicy.maximumEfficientDirectRepeatBlockCount)
    {
        return OuterFecResult<pbprotocol::OuterFecMode>::Success(
            pbprotocol::OuterFecMode::DirectRepeat);
    }
    if (blockCount <= kWirehairV2MaximumBlockCount)
    {
        return OuterFecResult<pbprotocol::OuterFecMode>::Success(
            pbprotocol::OuterFecMode::WirehairV2);
    }

    return OuterFecResult<pbprotocol::OuterFecMode>::Failure(
        OuterFecErrorCode::InvalidDimensions,
        blockCount);
}

DirectRepeatEncoder::DirectRepeatEncoder(
    DirectRepeatEncoder&& other) noexcept
    : encodedSegment_(std::move(other.encodedSegment_))
    , encodedSize_(other.encodedSize_)
    , blockCount_(other.blockCount_)
    , outerBlockBytes_(other.outerBlockBytes_)
{
    other.encodedSegment_.clear();
    other.encodedSize_ = 0;
    other.blockCount_ = 0;
    other.outerBlockBytes_ = 0;
    other.moved_ = true;
}

DirectRepeatEncoder& DirectRepeatEncoder::operator=(
    DirectRepeatEncoder&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    encodedSegment_ = std::move(other.encodedSegment_);
    encodedSize_ = other.encodedSize_;
    blockCount_ = other.blockCount_;
    outerBlockBytes_ = other.outerBlockBytes_;
    moved_ = false;

    other.encodedSegment_.clear();
    other.encodedSize_ = 0;
    other.blockCount_ = 0;
    other.outerBlockBytes_ = 0;
    other.moved_ = true;
    return *this;
}

OuterFecResult<DirectRepeatEncoder> DirectRepeatEncoder::Create(
    const std::span<const std::byte> encodedSegment,
    const std::uint32_t outerBlockBytes)
{
    if (!SizeFitsUint64(encodedSegment.size()))
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            std::numeric_limits<std::uint64_t>::max());
    }

    const std::uint64_t encodedSize = SizeToUint64(encodedSegment.size());
    const auto blockCountResult = GetDirectBlockCount(
        encodedSize, outerBlockBytes);
    if (!blockCountResult)
    {
        return FailureFrom<DirectRepeatEncoder>(blockCountResult.Error());
    }

    const std::vector<std::byte> emptyBytes;
    if (encodedSegment.size() > emptyBytes.max_size())
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            encodedSize);
    }

    DirectRepeatEncoder encoder;
    try
    {
        encoder.encodedSegment_.assign(
            encodedSegment.begin(), encodedSegment.end());
    }
    catch (const std::bad_alloc&)
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::OutOfMemory);
    }
    catch (const std::length_error&)
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            encodedSize);
    }

    encoder.encodedSize_ = encodedSize;
    encoder.blockCount_ = blockCountResult.Value();
    encoder.outerBlockBytes_ = outerBlockBytes;
    return OuterFecResult<DirectRepeatEncoder>::Success(
        std::move(encoder));
}

OuterFecResult<DirectRepeatEncoder> DirectRepeatEncoder::Recreate(
    const std::span<const std::byte> exactEncodedSegment,
    const pbprotocol::SegmentDescriptor& segmentDescriptor)
{
    const auto descriptorResult = ValidateDirectDescriptor(segmentDescriptor);
    if (!descriptorResult)
    {
        return FailureFrom<DirectRepeatEncoder>(descriptorResult.Error());
    }
    if (!SizeFitsUint64(exactEncodedSegment.size()))
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            std::numeric_limits<std::uint64_t>::max());
    }

    const std::uint64_t actualEncodedSize =
        SizeToUint64(exactEncodedSegment.size());
    if (actualEncodedSize != segmentDescriptor.encodedSize)
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::EncodedSizeMismatch,
            actualEncodedSize);
    }

    const pbprotocol::EncodedDigest actualDigest{
        pbprotocol::ComputeBlake3Digest(exactEncodedSegment)};
    if (actualDigest != segmentDescriptor.encodedDigest)
    {
        return OuterFecResult<DirectRepeatEncoder>::Failure(
            OuterFecErrorCode::EncodedDigestMismatch);
    }

    return Create(exactEncodedSegment, segmentDescriptor.outerBlockBytes);
}

OuterFecResult<std::uint32_t> DirectRepeatEncoder::EncodeBlock(
    const std::uint32_t outerBlockId,
    const std::span<std::byte> paddedOutput)
{
    if (moved_ || outerBlockBytes_ == 0)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (static_cast<std::uint64_t>(outerBlockId) >= blockCount_)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidInput,
            outerBlockId);
    }
    if (paddedOutput.size() < outerBlockBytes_)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::BufferTooSmall,
            outerBlockBytes_);
    }

    const auto blockOffsetResult = GetBlockOffset(
        outerBlockId, outerBlockBytes_);
    if (!blockOffsetResult || blockOffsetResult.Value() >= encodedSize_)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidState,
            outerBlockId);
    }
    const auto blockOffsetSizeResult = pbprotocol::CheckedUint64ToSize(
        blockOffsetResult.Value());
    if (!blockOffsetSizeResult)
    {
        return OuterFecResult<std::uint32_t>::Failure(
            OuterFecErrorCode::InvalidState,
            blockOffsetResult.Value());
    }

    const std::uint32_t payloadBytes = GetRequiredPayloadBytes(
        encodedSize_, blockOffsetResult.Value(), outerBlockBytes_);
    std::span<std::byte> canonicalOutput = paddedOutput.first(
        static_cast<std::size_t>(outerBlockBytes_));
    std::fill(canonicalOutput.begin(), canonicalOutput.end(), std::byte{0});
    const std::span<const std::byte> source(encodedSegment_);
    const std::span<const std::byte> sourceBlock = source.subspan(
        blockOffsetSizeResult.Value(),
        static_cast<std::size_t>(payloadBytes));
    std::copy(sourceBlock.begin(), sourceBlock.end(), canonicalOutput.begin());

    return OuterFecResult<std::uint32_t>::Success(payloadBytes);
}

std::uint64_t DirectRepeatEncoder::GetBlockCount() const noexcept
{
    return blockCount_;
}

std::uint64_t DirectRepeatEncoder::GetEncodedSize() const noexcept
{
    return encodedSize_;
}

std::uint32_t DirectRepeatEncoder::GetOuterBlockBytes() const noexcept
{
    return outerBlockBytes_;
}

DirectRepeatDecoder::DirectRepeatDecoder(
    DirectRepeatDecoder&& other) noexcept = default;

DirectRepeatDecoder& DirectRepeatDecoder::operator=(
    DirectRepeatDecoder&& other) noexcept = default;

DirectRepeatDecoder::~DirectRepeatDecoder() = default;

OuterFecResult<DirectRepeatDecoder> DirectRepeatDecoder::Create(
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
    const std::uint32_t expectedOuterBlockBytes,
    const OuterFecDecoderResourceManager& resourceManager)
{
    return CreateFromDescriptor(
        boundSegmentDescriptor.GetDescriptor(),
        expectedOuterBlockBytes,
        resourceManager);
}

OuterFecResult<DirectRepeatDecoder>
DirectRepeatDecoder::CreateFromDescriptor(
    const pbprotocol::SegmentDescriptor& segmentDescriptor,
    const std::uint32_t expectedOuterBlockBytes,
    const OuterFecDecoderResourceManager& resourceManager)
{
    return CreateFromDescriptor(
        segmentDescriptor,
        expectedOuterBlockBytes,
        resourceManager,
        false);
}

OuterFecResult<DirectRepeatDecoder>
DirectRepeatDecoder::CreateFromDescriptor(
    const pbprotocol::SegmentDescriptor& segmentDescriptor,
    const std::uint32_t expectedOuterBlockBytes,
    const OuterFecDecoderResourceManager& resourceManager,
    const bool forceAllocationFailureAfterReservation)
{
    if (resourceManager.state_ == nullptr)
    {
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (expectedOuterBlockBytes == 0 ||
        expectedOuterBlockBytes >
            pbprotocol::kMaximumTransportPayloadBytes)
    {
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::InvalidDimensions,
            expectedOuterBlockBytes);
    }
    if (segmentDescriptor.outerBlockBytes != expectedOuterBlockBytes)
    {
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OuterBlockBytesMismatch,
            segmentDescriptor.outerBlockBytes);
    }

    const pbprotocol::ReceiverResourcePolicy& resourcePolicy =
        resourceManager.state_->resourcePolicy;
    if (segmentDescriptor.encodedSize >
        resourcePolicy.maxEncodedSegmentBytes)
    {
        detail::CountOuterFecDecoderQuotaExceeded(resourceManager.state_);
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            segmentDescriptor.encodedSize);
    }
    if (segmentDescriptor.outerBlockBytes >
        resourcePolicy.maxOuterBlockBytes)
    {
        detail::CountOuterFecDecoderQuotaExceeded(resourceManager.state_);
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            segmentDescriptor.outerBlockBytes);
    }

    const auto descriptorResult = ValidateDirectDescriptor(segmentDescriptor);
    if (!descriptorResult)
    {
        return FailureFrom<DirectRepeatDecoder>(descriptorResult.Error());
    }
    const std::uint64_t blockCount = descriptorResult.Value().blockCount;
    if (blockCount > resourcePolicy.maxDirectRepeatBlockCount)
    {
        detail::CountOuterFecDecoderQuotaExceeded(resourceManager.state_);
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OuterFecDecoderQuotaExceeded,
            blockCount);
    }

    const auto estimateResult = CalculateDirectRepeatResourceEstimate(
        segmentDescriptor.encodedSize, blockCount);
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
        return FailureFrom<DirectRepeatDecoder>(estimateResult.Error());
    }
    const DirectRepeatResourceEstimate& estimate = estimateResult.Value();

    auto reservationResult = detail::AcquireOuterFecDecoderReservation(
        resourceManager.state_, estimate.reservationBytes);
    if (!reservationResult)
    {
        return FailureFrom<DirectRepeatDecoder>(reservationResult.Error());
    }
    detail::OuterFecDecoderReservation reservation =
        std::move(reservationResult).Value();

    std::unique_ptr<detail::DirectRepeatDecoderImplementation> implementation;
    try
    {
        if (forceAllocationFailureAfterReservation)
        {
            throw std::bad_alloc{};
        }
        implementation =
            std::make_unique<detail::DirectRepeatDecoderImplementation>();
        implementation->encodedBytes.resize(estimate.encodedSize);
        implementation->receivedBitmap.resize(estimate.bitmapWordCount, 0);
        implementation->resourceReservation = std::move(reservation);
    }
    catch (const std::bad_alloc&)
    {
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OutOfMemory);
    }
    catch (const std::length_error&)
    {
        return OuterFecResult<DirectRepeatDecoder>::Failure(
            OuterFecErrorCode::OutOfMemory,
            segmentDescriptor.encodedSize);
    }

    implementation->expectedDigest = segmentDescriptor.encodedDigest;
    implementation->encodedSize = segmentDescriptor.encodedSize;
    implementation->blockCount = blockCount;
    implementation->outerBlockBytes = segmentDescriptor.outerBlockBytes;

    DirectRepeatDecoder decoder;
    decoder.implementation_ = std::move(implementation);
    return OuterFecResult<DirectRepeatDecoder>::Success(std::move(decoder));
}

std::uint64_t DirectRepeatDecoder::GetBoundBlockCount() const noexcept
{
    if (implementation_ == nullptr)
    {
        return 0;
    }
    return implementation_->blockCount;
}

std::uint64_t DirectRepeatDecoder::GetAcceptedBlockCount() const noexcept
{
    return implementation_ == nullptr ? 0 : implementation_->receivedBlockCount;
}

OuterFecResult<DecodeDisposition> DirectRepeatDecoder::DecodeBlock(
    const std::uint32_t outerBlockId,
    const std::uint32_t payloadBytes,
    const std::span<const std::byte> paddedPayload)
{
    if (implementation_ == nullptr)
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

    const auto failTerminal = [this](
        const OuterFecErrorCode code,
        const std::uint64_t detailValue = 0)
    {
        const OuterFecError error{code, detailValue};
        implementation_->terminalError = error;
        return FailureFrom<DecodeDisposition>(error);
    };

    if (paddedPayload.size() != implementation_->outerBlockBytes)
    {
        return failTerminal(
            OuterFecErrorCode::InvalidInput,
            SizeToUint64(paddedPayload.size()));
    }
    if (static_cast<std::uint64_t>(outerBlockId) >=
        implementation_->blockCount)
    {
        return failTerminal(
            OuterFecErrorCode::InvalidInput,
            outerBlockId);
    }

    const auto blockOffsetResult = GetBlockOffset(
        outerBlockId, implementation_->outerBlockBytes);
    if (!blockOffsetResult ||
        blockOffsetResult.Value() >= implementation_->encodedSize)
    {
        return failTerminal(
            OuterFecErrorCode::InvalidState,
            outerBlockId);
    }
    const std::uint32_t expectedPayloadBytes = GetRequiredPayloadBytes(
        implementation_->encodedSize,
        blockOffsetResult.Value(),
        implementation_->outerBlockBytes);
    if (payloadBytes != expectedPayloadBytes)
    {
        return failTerminal(
            OuterFecErrorCode::InvalidInput,
            payloadBytes);
    }

    const std::size_t payloadByteCount =
        static_cast<std::size_t>(payloadBytes);
    for (std::size_t paddingIndex = payloadByteCount;
        paddingIndex < paddedPayload.size();
        paddingIndex++)
    {
        if (paddedPayload[paddingIndex] != std::byte{0})
        {
            return failTerminal(
                OuterFecErrorCode::InvalidInput,
                SizeToUint64(paddingIndex));
        }
    }

    const auto blockOffsetSizeResult = pbprotocol::CheckedUint64ToSize(
        blockOffsetResult.Value());
    if (!blockOffsetSizeResult)
    {
        return failTerminal(
            OuterFecErrorCode::InvalidState,
            blockOffsetResult.Value());
    }
    const std::size_t blockOffset = blockOffsetSizeResult.Value();
    const std::size_t bitmapWordIndex = static_cast<std::size_t>(
        static_cast<std::uint64_t>(outerBlockId) /
        kReceivedBitmapWordBits);
    const std::uint32_t bitIndex = outerBlockId %
        static_cast<std::uint32_t>(kReceivedBitmapWordBits);
    const std::uint64_t receivedMask = 1ULL << bitIndex;
    std::uint64_t& receivedWord =
        implementation_->receivedBitmap[bitmapWordIndex];

    const std::span<const std::byte> realPayload =
        paddedPayload.first(payloadByteCount);
    std::span<std::byte> encodedBytes(implementation_->encodedBytes);
    std::span<std::byte> destination = encodedBytes.subspan(
        blockOffset, payloadByteCount);
    if ((receivedWord & receivedMask) != 0)
    {
        if (!std::equal(
                destination.begin(), destination.end(), realPayload.begin()))
        {
            return failTerminal(
                OuterFecErrorCode::OuterBlockConflict,
                outerBlockId);
        }
        return OuterFecResult<DecodeDisposition>::Success(
            implementation_->ready
                ? DecodeDisposition::Ready
                : DecodeDisposition::NeedMore);
    }

    std::copy(realPayload.begin(), realPayload.end(), destination.begin());
    receivedWord |= receivedMask;
    implementation_->receivedBlockCount++;

    if (implementation_->receivedBlockCount == implementation_->blockCount)
    {
        const pbprotocol::EncodedDigest actualDigest{
            pbprotocol::ComputeBlake3Digest(implementation_->encodedBytes)};
        if (actualDigest != implementation_->expectedDigest)
        {
            return failTerminal(OuterFecErrorCode::EncodedDigestMismatch);
        }
        implementation_->ready = true;
        return OuterFecResult<DecodeDisposition>::Success(
            DecodeDisposition::Ready);
    }

    return OuterFecResult<DecodeDisposition>::Success(
        DecodeDisposition::NeedMore);
}

OuterFecResult<std::uint64_t> DirectRepeatDecoder::Recover(
    const std::span<std::byte> output)
{
    if (implementation_ == nullptr ||
        !implementation_->ready ||
        implementation_->terminalError.has_value())
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::InvalidState);
    }
    if (output.size() < implementation_->encodedBytes.size())
    {
        return OuterFecResult<std::uint64_t>::Failure(
            OuterFecErrorCode::BufferTooSmall,
            implementation_->encodedSize);
    }

    std::copy(
        implementation_->encodedBytes.begin(),
        implementation_->encodedBytes.end(),
        output.begin());
    return OuterFecResult<std::uint64_t>::Success(
        implementation_->encodedSize);
}

} // namespace pbouterfec
