#include "pbreceiver/receiver_ingress.h"

#include "pbcompression/segment_decompression.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/outer_fec_decoder_resource.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/orphan_transport_block_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pbreceiver
{

namespace detail
{

struct ReceiverSegmentKey
{
    std::uint64_t sessionTagValue = 0;
    std::uint64_t segmentOrdinal = 0;

    bool operator<(const ReceiverSegmentKey& other) const noexcept
    {
        if (sessionTagValue != other.sessionTagValue)
        {
            return sessionTagValue < other.sessionTagValue;
        }
        return segmentOrdinal < other.segmentOrdinal;
    }
};

using ReceiverDecoder = std::variant<
    pbouterfec::DirectRepeatDecoder,
    pbouterfec::WirehairV2Decoder>;

struct ActiveDecoderState
{
    ActiveDecoderState(
        pbprotocol::BoundSegmentDescriptor boundDescriptor,
        pbouterfec::DirectRepeatDecoder decoder)
        : boundSegmentDescriptor(std::move(boundDescriptor)),
          decoderState(
              std::in_place_type<pbouterfec::DirectRepeatDecoder>,
              std::move(decoder))
    {
    }

    ActiveDecoderState(
        pbprotocol::BoundSegmentDescriptor boundDescriptor,
        pbouterfec::WirehairV2Decoder decoder)
        : boundSegmentDescriptor(std::move(boundDescriptor)),
          decoderState(
              std::in_place_type<pbouterfec::WirehairV2Decoder>,
              std::move(decoder))
    {
    }

    ActiveDecoderState(const ActiveDecoderState&) = delete;
    ActiveDecoderState& operator=(const ActiveDecoderState&) = delete;
    ActiveDecoderState(ActiveDecoderState&&) noexcept = default;
    ActiveDecoderState& operator=(ActiveDecoderState&&) noexcept = default;

    pbprotocol::BoundSegmentDescriptor boundSegmentDescriptor;
    ReceiverDecoder decoderState;
    bool encodedSegmentReady = false;
};

struct ReceiverIngressImplementation
{
    ReceiverIngressImplementation(
        pbprotocol::ReceiverResourcePolicy receiverResourcePolicy,
        const std::uint32_t profileOuterBlockBytes,
        pbprotocol::ControlPlaneReceiver controlPlaneReceiver,
        pbprotocol::OrphanTransportBlockCache orphanTransportBlockCache,
        pbouterfec::OuterFecDecoderResourceManager decoderResourceManager,
        const pbcompression::DecompressionLimits& receiverDecompressionLimits)
        : resourcePolicy(std::move(receiverResourcePolicy)),
          expectedOuterBlockBytes(profileOuterBlockBytes),
          controlReceiver(std::move(controlPlaneReceiver)),
          orphanCache(std::move(orphanTransportBlockCache)),
          outerFecResourceManager(std::move(decoderResourceManager)),
          decompressionLimits(receiverDecompressionLimits)
    {
    }

    const pbprotocol::ReceiverResourcePolicy resourcePolicy;
    std::uint32_t expectedOuterBlockBytes = 0;
    pbprotocol::ControlPlaneReceiver controlReceiver;
    pbprotocol::OrphanTransportBlockCache orphanCache;
    // activeDecoders is declared after the manager so reverse member
    // destruction releases every reservation before the manager object.
    pbouterfec::OuterFecDecoderResourceManager outerFecResourceManager;
    const pbcompression::DecompressionLimits decompressionLimits{};
    std::map<ReceiverSegmentKey, ActiveDecoderState> activeDecoders;
    std::map<std::uint64_t, ReceiverError> terminalSessionErrors;
    // Allocation failure while recording a per-session terminal error must
    // still fail closed. This allocation-free fallback stops all admissions
    // until a capture-epoch reset rebuilds the receiver state.
    std::optional<ReceiverError> globalTerminalError;

    std::uint64_t totalResourcePolicyRejectedCount = 0;
    std::uint64_t orphanConflictRejectionCount = 0;
    std::uint64_t resumeQuotaRejectedCount = 0;
    std::uint64_t decompressionResourceRejectedCount = 0;
    std::uint64_t decompressionInputQuotaRejectedCount = 0;
    std::uint64_t decompressionOutputQuotaRejectedCount = 0;
    std::uint64_t decompressionWindowQuotaRejectedCount = 0;
    std::uint64_t decompressionAllocationFailureCount = 0;
    std::uint64_t outputReservationDeniedCount = 0;
    std::uint64_t outputReservationRequiresConfirmationCount = 0;
    std::uint64_t outputReservationAutoAcceptedCount = 0;

    // Component counters are cumulative only for one component lifetime.
    // Capture-epoch reset snapshots them here before replacing the component.
    std::uint64_t priorControlRejectedCount = 0;
    std::uint64_t priorOuterFecQuotaExceededCount = 0;
    std::uint64_t deferredResourceBusyCount = 0;
    std::uint64_t priorOrphanAdmittedCount = 0;
    std::uint64_t priorOrphanDroppedCount = 0;
    std::uint64_t priorOrphanResourceExhaustedCount = 0;
};

} // namespace detail

namespace
{

template <typename ValueType>
[[nodiscard]] ReceiverResult<ValueType> FailureFrom(
    const pbprotocol::ProtocolError& error)
{
    return ReceiverResult<ValueType>::Failure(ReceiverError{error});
}

template <typename ValueType>
[[nodiscard]] ReceiverResult<ValueType> FailureFrom(
    const pbouterfec::OuterFecError& error)
{
    return ReceiverResult<ValueType>::Failure(ReceiverError{error});
}

template <typename ValueType>
[[nodiscard]] ReceiverResult<ValueType> FailureFrom(
    const pbcompression::CompressionError& error)
{
    return ReceiverResult<ValueType>::Failure(ReceiverError{error});
}

[[nodiscard]] bool IsProtocolResourceFailure(
    const pbprotocol::ProtocolErrorCode errorCode) noexcept
{
    return errorCode == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded ||
        errorCode == pbprotocol::ProtocolErrorCode::ResourceExhausted ||
        errorCode ==
            pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded ||
        errorCode == pbprotocol::ProtocolErrorCode::OutputReservationDenied;
}

[[nodiscard]] bool IsOuterFecResourceFailure(
    const pbouterfec::OuterFecErrorCode errorCode) noexcept
{
    return errorCode ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded ||
        errorCode == pbouterfec::OuterFecErrorCode::OutOfMemory;
}

[[nodiscard]] bool IsCompressionResourceFailure(
    const pbcompression::CompressionErrorCode errorCode) noexcept
{
    return errorCode == pbcompression::CompressionErrorCode::OutputLimitExceeded ||
        errorCode == pbcompression::CompressionErrorCode::InputLimitExceeded ||
        errorCode == pbcompression::CompressionErrorCode::WindowLimitExceeded ||
        errorCode ==
            pbcompression::CompressionErrorCode::CompressedFrameLimitExceeded ||
        errorCode == pbcompression::CompressionErrorCode::AllocationFailure;
}

[[nodiscard]] bool IsResourceFailure(const ReceiverError& error) noexcept
{
    return std::visit(
        [](const auto& typedError) noexcept
        {
            using ErrorType = std::decay_t<decltype(typedError)>;
            if constexpr (std::is_same_v<ErrorType, pbprotocol::ProtocolError>)
            {
                return IsProtocolResourceFailure(typedError.code);
            }
            else if constexpr (
                std::is_same_v<ErrorType, pbouterfec::OuterFecError>)
            {
                return IsOuterFecResourceFailure(typedError.code);
            }
            else
            {
                return IsCompressionResourceFailure(typedError.code);
            }
        },
        error);
}

void CountReturnedResourceFailure(
    detail::ReceiverIngressImplementation& implementation,
    const ReceiverError& error) noexcept
{
    if (IsResourceFailure(error))
    {
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.totalResourcePolicyRejectedCount);
    }
}

void CountDecompressionResourceFailure(
    detail::ReceiverIngressImplementation& implementation,
    const pbcompression::CompressionErrorCode errorCode) noexcept
{
    if (!IsCompressionResourceFailure(errorCode))
    {
        return;
    }

    pbprotocol::SaturatingIncrementUnsigned(
        implementation.decompressionResourceRejectedCount);
    switch (errorCode)
    {
    case pbcompression::CompressionErrorCode::InputLimitExceeded:
    case pbcompression::CompressionErrorCode::CompressedFrameLimitExceeded:
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.decompressionInputQuotaRejectedCount);
        break;
    case pbcompression::CompressionErrorCode::OutputLimitExceeded:
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.decompressionOutputQuotaRejectedCount);
        break;
    case pbcompression::CompressionErrorCode::WindowLimitExceeded:
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.decompressionWindowQuotaRejectedCount);
        break;
    case pbcompression::CompressionErrorCode::AllocationFailure:
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.decompressionAllocationFailureCount);
        break;
    default:
        break;
    }
}

[[nodiscard]] ReceiverResult<bool> ValidateProfileBlockBytes(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint32_t expectedOuterBlockBytes)
{
    if (expectedOuterBlockBytes == 0 ||
        expectedOuterBlockBytes > resourcePolicy.maxOuterBlockBytes ||
        expectedOuterBlockBytes >
            pbprotocol::kMaximumTransportPayloadBytes)
    {
        return FailureFrom<bool>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidResourcePolicy,
            0});
    }
    return ReceiverResult<bool>::Success(true);
}

[[nodiscard]] ReceiverResult<bool> ValidateTransportBlockShape(
    const ReceivedTransportBlock& transportBlock,
    const std::uint32_t expectedOuterBlockBytes)
{
    if (transportBlock.paddedPayload.size() != expectedOuterBlockBytes ||
        transportBlock.declaredPayloadBytes == 0 ||
        transportBlock.declaredPayloadBytes >
            transportBlock.paddedPayload.size())
    {
        return FailureFrom<bool>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidRecordSize,
            0});
    }

    const std::size_t declaredPayloadBytes =
        transportBlock.declaredPayloadBytes;
    for (std::size_t paddingIndex = declaredPayloadBytes;
         paddingIndex < transportBlock.paddedPayload.size();
         paddingIndex++)
    {
        if (transportBlock.paddedPayload[paddingIndex] != std::byte{0})
        {
            return FailureFrom<bool>(pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::NonCanonicalPadding,
                paddingIndex});
        }
    }
    return ReceiverResult<bool>::Success(true);
}

[[nodiscard]] std::optional<ReceiverError> FindTerminalSessionError(
    const detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::SessionTag sessionTag)
{
    if (implementation.globalTerminalError)
    {
        return implementation.globalTerminalError;
    }

    const auto iterator = implementation.terminalSessionErrors.find(
        sessionTag.value);
    if (iterator == implementation.terminalSessionErrors.end())
    {
        return std::nullopt;
    }
    return iterator->second;
}

void EraseActiveDecodersForSession(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::SessionTag sessionTag) noexcept
{
    for (auto iterator = implementation.activeDecoders.begin();
         iterator != implementation.activeDecoders.end();)
    {
        if (iterator->first.sessionTagValue != sessionTag.value)
        {
            iterator++;
            continue;
        }
        iterator = implementation.activeDecoders.erase(iterator);
    }
}

void ClearAllDataPlaneState(
    detail::ReceiverIngressImplementation& implementation) noexcept
{
    implementation.activeDecoders.clear();
    implementation.orphanCache.ClearAll();
}

void ClearLastTerminalControlSessionData(
    detail::ReceiverIngressImplementation& implementation) noexcept
{
    const std::optional<pbprotocol::SessionTag> sessionTag =
        implementation.controlReceiver.GetLastTerminalSessionTag();
    if (!sessionTag)
    {
        return;
    }
    EraseActiveDecodersForSession(implementation, *sessionTag);
    implementation.orphanCache.ClearSession(*sessionTag);
}

[[nodiscard]] ReceiverError LatchTerminalSessionError(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::SessionTag sessionTag,
    ReceiverError error)
{
    const auto existing = implementation.terminalSessionErrors.find(
        sessionTag.value);
    if (existing != implementation.terminalSessionErrors.end())
    {
        return existing->second;
    }

    try
    {
        const auto insertion = implementation.terminalSessionErrors.emplace(
            sessionTag.value,
            error);
        EraseActiveDecodersForSession(implementation, sessionTag);
        implementation.orphanCache.ClearSession(sessionTag);
        return insertion.first->second;
    }
    catch (const std::bad_alloc&)
    {
        implementation.globalTerminalError = ReceiverError{
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted,
                0}};
        implementation.terminalSessionErrors.clear();
        ClearAllDataPlaneState(implementation);
        return *implementation.globalTerminalError;
    }
    catch (const std::length_error&)
    {
        implementation.globalTerminalError = ReceiverError{
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted,
                0}};
        implementation.terminalSessionErrors.clear();
        ClearAllDataPlaneState(implementation);
        return *implementation.globalTerminalError;
    }
}

[[nodiscard]] bool IsRetryableOuterFecError(
    const pbouterfec::OuterFecErrorCode errorCode) noexcept
{
    return errorCode ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded ||
        errorCode == pbouterfec::OuterFecErrorCode::OutOfMemory ||
        errorCode == pbouterfec::OuterFecErrorCode::CsprngFailure;
}

[[nodiscard]] bool IsOuterFecDecoderQuotaError(
    const pbouterfec::OuterFecError& error) noexcept
{
    return error.code == pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded;
}

template <typename ValueType>
[[nodiscard]] ReceiverResult<ValueType> FailDecoderCreationForSession(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::SessionTag sessionTag,
    const pbouterfec::OuterFecError& error)
{
    if (IsRetryableOuterFecError(error.code))
    {
        return FailureFrom<ValueType>(error);
    }
    return ReceiverResult<ValueType>::Failure(LatchTerminalSessionError(
        implementation,
        sessionTag,
        ReceiverError{error}));
}

template <typename ValueType>
[[nodiscard]] ReceiverResult<ValueType> FailActiveDecoderForSession(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::SessionTag sessionTag,
    const pbouterfec::OuterFecError& error)
{
    // Decoder methods latch every reported error internally, including
    // OutOfMemory. Never retain or retry a terminal third-party/wrapper codec
    // instance merely because the same code is retryable during Create().
    return ReceiverResult<ValueType>::Failure(LatchTerminalSessionError(
        implementation,
        sessionTag,
        ReceiverError{error}));
}

[[nodiscard]] ReceiverResult<detail::ActiveDecoderState*>
EnsureActiveDecoder(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor)
{
    const pbprotocol::SegmentDescriptor& descriptor =
        boundSegmentDescriptor.GetDescriptor();
    const detail::ReceiverSegmentKey key{
        descriptor.sessionTag.value,
        descriptor.segmentOrdinal};
    const auto existing = implementation.activeDecoders.find(key);
    if (existing != implementation.activeDecoders.end())
    {
        return ReceiverResult<detail::ActiveDecoderState*>::Success(
            &existing->second);
    }

    if (descriptor.outerBlockBytes != implementation.expectedOuterBlockBytes)
    {
        return FailureFrom<detail::ActiveDecoderState*>(
            pbouterfec::OuterFecError{
                pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch,
                descriptor.outerBlockBytes});
    }

    try
    {
        if (descriptor.outerFecMode == pbprotocol::OuterFecMode::DirectRepeat)
        {
            auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
                boundSegmentDescriptor,
                implementation.expectedOuterBlockBytes,
                implementation.outerFecResourceManager);
            if (!decoderResult)
            {
                return FailureFrom<detail::ActiveDecoderState*>(
                    decoderResult.Error());
            }
            auto insertion = implementation.activeDecoders.emplace(
                key,
                detail::ActiveDecoderState(
                    boundSegmentDescriptor,
                    std::move(decoderResult).Value()));
            return ReceiverResult<detail::ActiveDecoderState*>::Success(
                &insertion.first->second);
        }
        if (descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
        {
            auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
                boundSegmentDescriptor,
                implementation.expectedOuterBlockBytes,
                implementation.outerFecResourceManager);
            if (!decoderResult)
            {
                return FailureFrom<detail::ActiveDecoderState*>(
                    decoderResult.Error());
            }
            auto insertion = implementation.activeDecoders.emplace(
                key,
                detail::ActiveDecoderState(
                    boundSegmentDescriptor,
                    std::move(decoderResult).Value()));
            return ReceiverResult<detail::ActiveDecoderState*>::Success(
                &insertion.first->second);
        }
        return FailureFrom<detail::ActiveDecoderState*>(
            pbouterfec::OuterFecError{
                pbouterfec::OuterFecErrorCode::InvalidDescriptor,
                static_cast<std::uint64_t>(descriptor.outerFecMode)});
    }
    catch (const std::bad_alloc&)
    {
        return FailureFrom<detail::ActiveDecoderState*>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted,
                0});
    }
    catch (const std::length_error&)
    {
        return FailureFrom<detail::ActiveDecoderState*>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted,
                0});
    }
}

[[nodiscard]] pbouterfec::OuterFecResult<pbouterfec::DecodeDisposition>
DecodeBlock(
    detail::ActiveDecoderState& activeDecoder,
    const ReceivedTransportBlock& transportBlock)
{
    if (std::holds_alternative<pbouterfec::DirectRepeatDecoder>(
            activeDecoder.decoderState))
    {
        return std::get<pbouterfec::DirectRepeatDecoder>(
            activeDecoder.decoderState).
            DecodeBlock(
                transportBlock.outerBlockId,
                transportBlock.declaredPayloadBytes,
                transportBlock.paddedPayload);
    }

    const std::size_t payloadBytes = transportBlock.declaredPayloadBytes;
    const auto decodeResult = std::get<pbouterfec::WirehairV2Decoder>(
        activeDecoder.decoderState).
        DecodeBlock(
            transportBlock.outerBlockId,
            transportBlock.paddedPayload.first(payloadBytes));
    if (activeDecoder.encodedSegmentReady && !decodeResult &&
        decodeResult.Error().code == pbouterfec::OuterFecErrorCode::InvalidState)
    {
        // A ready Wirehair decoder rejects an unseen equation without adding
        // it to its fingerprint table. Keep the verified recovery available
        // while storage is pending; Recover still checks the codec and digest.
        // Known IDs continue through DecodeBlock's duplicate/conflict checks.
        return pbouterfec::OuterFecResult<pbouterfec::DecodeDisposition>::Success(
            pbouterfec::DecodeDisposition::Ready);
    }
    return decodeResult;
}

[[nodiscard]] std::uint64_t GetAcceptedBlockCount(
    const detail::ActiveDecoderState& activeDecoder) noexcept
{
    if (std::holds_alternative<pbouterfec::DirectRepeatDecoder>(activeDecoder.decoderState))
    {
        return std::get<pbouterfec::DirectRepeatDecoder>(activeDecoder.decoderState).GetAcceptedBlockCount();
    }
    return std::get<pbouterfec::WirehairV2Decoder>(activeDecoder.decoderState).GetAcceptedBlockCount();
}

[[nodiscard]] pbouterfec::OuterFecResult<std::uint64_t> RecoverSegment(
    detail::ActiveDecoderState& activeDecoder,
    const std::span<std::byte> output)
{
    if (std::holds_alternative<pbouterfec::DirectRepeatDecoder>(
            activeDecoder.decoderState))
    {
        return std::get<pbouterfec::DirectRepeatDecoder>(
            activeDecoder.decoderState).Recover(output);
    }
    return std::get<pbouterfec::WirehairV2Decoder>(
        activeDecoder.decoderState).Recover(output);
}

[[nodiscard]] ReceiverResult<ReceiverDataAdmission> ProcessBoundDataBlock(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
    const ReceivedTransportBlock& transportBlock)
{
    const pbprotocol::SegmentDescriptor& descriptor =
        boundSegmentDescriptor.GetDescriptor();
    const auto completedResult = implementation.controlReceiver.
        IsSegmentCompleted(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!completedResult)
    {
        return FailureFrom<ReceiverDataAdmission>(completedResult.Error());
    }
    if (completedResult.Value())
    {
        return ReceiverResult<ReceiverDataAdmission>::Success(
            ReceiverDataAdmission{
                ReceiverDataDisposition::AlreadyCompleted,
                std::nullopt,
                ReceiverOuterSymbolAdmission::AlreadyCompleted});
    }

    auto activeDecoderResult = EnsureActiveDecoder(
        implementation,
        boundSegmentDescriptor);
    if (!activeDecoderResult)
    {
        if (const auto* outerFecError =
                std::get_if<pbouterfec::OuterFecError>(
                    &activeDecoderResult.Error()))
        {
            if (IsOuterFecDecoderQuotaError(*outerFecError))
            {
                pbprotocol::SaturatingIncrementUnsigned(implementation.deferredResourceBusyCount);
                return ReceiverResult<ReceiverDataAdmission>::Success(
                    ReceiverDataAdmission{
                        ReceiverDataDisposition::DeferredResourceBusy,
                        std::nullopt,
                        ReceiverOuterSymbolAdmission::DeferredResourceBusy});
            }
            return FailDecoderCreationForSession<ReceiverDataAdmission>(
                implementation,
                descriptor.sessionTag,
                *outerFecError);
        }
        return ReceiverResult<ReceiverDataAdmission>::Failure(
            activeDecoderResult.Error());
    }

    detail::ActiveDecoderState& activeDecoder =
        *activeDecoderResult.Value();
    const std::uint64_t acceptedBlocksBefore = GetAcceptedBlockCount(activeDecoder);
    const bool recoveryWasReady = activeDecoder.encodedSegmentReady;
    const auto decodeResult = DecodeBlock(activeDecoder, transportBlock);
    if (!decodeResult)
    {
        return FailActiveDecoderForSession<ReceiverDataAdmission>(
            implementation,
            descriptor.sessionTag,
            decodeResult.Error());
    }
    const std::uint64_t acceptedBlocksAfter = GetAcceptedBlockCount(activeDecoder);
    const ReceiverOuterSymbolAdmission outerAdmission = acceptedBlocksAfter > acceptedBlocksBefore ?
        ReceiverOuterSymbolAdmission::Unique : recoveryWasReady ?
            ReceiverOuterSymbolAdmission::RecoveryAlreadyReady :
            ReceiverOuterSymbolAdmission::IdenticalDuplicate;
    if (decodeResult.Value() == pbouterfec::DecodeDisposition::NeedMore)
    {
        return ReceiverResult<ReceiverDataAdmission>::Success(
            ReceiverDataAdmission{
                ReceiverDataDisposition::AcceptedNeedMore,
                std::nullopt,
                outerAdmission});
    }
    activeDecoder.encodedSegmentReady = true;

    const auto recoveredSizeResult = pbprotocol::CheckedUint64ToSize(
        descriptor.encodedSize);
    if (!recoveredSizeResult)
    {
        return ReceiverResult<ReceiverDataAdmission>::Failure(
            LatchTerminalSessionError(
                implementation,
                descriptor.sessionTag,
                ReceiverError{recoveredSizeResult.Error()}));
    }

    std::vector<std::byte> recoveredBytes;
    try
    {
        recoveredBytes.resize(recoveredSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        // Retain the bounded ready decoder and its accepted-ID fingerprints.
        // A later observation can retry allocation without losing conflicts
        // that still matter until verified storage has been committed.
        return FailureFrom<ReceiverDataAdmission>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::ResourceExhausted,
            0});
    }
    catch (const std::length_error&)
    {
        return FailureFrom<ReceiverDataAdmission>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::ResourceExhausted,
            0});
    }

    const auto recoverResult = RecoverSegment(activeDecoder, recoveredBytes);
    if (!recoverResult)
    {
        return FailActiveDecoderForSession<ReceiverDataAdmission>(
            implementation,
            descriptor.sessionTag,
            recoverResult.Error());
    }
    if (recoverResult.Value() != descriptor.encodedSize)
    {
        return FailActiveDecoderForSession<ReceiverDataAdmission>(
            implementation,
            descriptor.sessionTag,
            pbouterfec::OuterFecError{
                pbouterfec::OuterFecErrorCode::EncodedSizeMismatch,
                recoverResult.Value()});
    }

    // Storage commit, not outer recovery, releases this decoder's reservation
    // and duplicate/conflict history. Verification or writes may still fail.
    return ReceiverResult<ReceiverDataAdmission>::Success(
        ReceiverDataAdmission{
            ReceiverDataDisposition::EncodedSegmentReady,
            ReceiverCompletedSegment(
                boundSegmentDescriptor,
                std::move(recoveredBytes)),
            outerAdmission});
}

struct OrphanReplayOutcome
{
    std::vector<pbprotocol::OrphanTransportBlockEntry> acceptedBlocks;
    std::optional<ReceiverCompletedSegment> completedSegment;
};

[[nodiscard]] ReceiverResult<OrphanReplayOutcome>
ProcessOrphanBlocks(
    detail::ReceiverIngressImplementation& implementation,
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor)
{
    const pbprotocol::SegmentDescriptor& descriptor =
        boundSegmentDescriptor.GetDescriptor();
    if (!implementation.orphanCache.HasCachedKey(
            descriptor.sessionTag,
            descriptor.segmentOrdinal))
    {
        return ReceiverResult<OrphanReplayOutcome>::Success({});
    }

    const auto completedResult = implementation.controlReceiver.
        IsSegmentCompleted(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!completedResult)
    {
        return FailureFrom<OrphanReplayOutcome>(
            completedResult.Error());
    }

    if (!completedResult.Value())
    {
        auto activeDecoderResult = EnsureActiveDecoder(
            implementation,
            boundSegmentDescriptor);
        if (!activeDecoderResult)
        {
            if (const auto* outerFecError =
                    std::get_if<pbouterfec::OuterFecError>(
                        &activeDecoderResult.Error()))
            {
                if (IsOuterFecDecoderQuotaError(*outerFecError))
                {
                    pbprotocol::SaturatingIncrementUnsigned(implementation.deferredResourceBusyCount);
                    return ReceiverResult<OrphanReplayOutcome>::Success({});
                }
                return FailDecoderCreationForSession<OrphanReplayOutcome>(
                    implementation,
                    descriptor.sessionTag,
                    *outerFecError);
            }
            return ReceiverResult<OrphanReplayOutcome>::Failure(
                activeDecoderResult.Error());
        }
    }

    auto drainResult = implementation.orphanCache.Drain(
        descriptor.sessionTag,
        descriptor.segmentOrdinal);
    if (!drainResult)
    {
        if (drainResult.Error().code ==
            pbprotocol::ProtocolErrorCode::OrphanPayloadConflict)
        {
            pbprotocol::SaturatingIncrementUnsigned(
                implementation.orphanConflictRejectionCount);
        }
        return ReceiverResult<OrphanReplayOutcome>::Failure(
            LatchTerminalSessionError(
                implementation,
                descriptor.sessionTag,
                ReceiverError{drainResult.Error()}));
    }

    if (completedResult.Value())
    {
        return ReceiverResult<OrphanReplayOutcome>::Success({});
    }

    pbprotocol::OrphanTransportBlockDrain drainedBlocks = std::move(drainResult).Value();
    std::size_t acceptedBlockCount = 0;
    OrphanReplayOutcome outcome;
    for (std::size_t entryIndex = 0; entryIndex < drainedBlocks.entries.size(); entryIndex++)
    {
        pbprotocol::OrphanTransportBlockEntry& entry = drainedBlocks.entries[entryIndex];
        const ReceivedTransportBlock transportBlock{
            descriptor.sessionTag,
            descriptor.segmentOrdinal,
            entry.outerBlockId,
            entry.declaredPayloadBytes,
            entry.paddedPayload};
        auto admissionResult = ProcessBoundDataBlock(
            implementation,
            boundSegmentDescriptor,
            transportBlock);
        if (!admissionResult)
        {
            return ReceiverResult<OrphanReplayOutcome>::Failure(admissionResult.Error());
        }
        if (admissionResult.Value().outerSymbolAdmission == ReceiverOuterSymbolAdmission::Unique)
        {
            if (acceptedBlockCount != entryIndex)
            {
                drainedBlocks.entries[acceptedBlockCount] = std::move(entry);
            }
            acceptedBlockCount++;
        }
        if (admissionResult.Value().completedSegment)
        {
            outcome.completedSegment = std::move(admissionResult.Value().completedSegment);
            break;
        }
    }
    drainedBlocks.entries.resize(acceptedBlockCount);
    outcome.acceptedBlocks = std::move(drainedBlocks.entries);
    return ReceiverResult<OrphanReplayOutcome>::Success(std::move(outcome));
}

[[nodiscard]] ReceiverResult<pbprotocol::OutputReservationDecision>
EvaluateOutputReservationCore(
    detail::ReceiverIngressImplementation& implementation,
    const std::uint64_t originalFileSize)
{
    const auto reservationResult = pbprotocol::EvaluateOutputReservation(
        originalFileSize,
        implementation.resourcePolicy);
    if (!reservationResult)
    {
        if (reservationResult.Error().code ==
            pbprotocol::ProtocolErrorCode::OutputReservationDenied)
        {
            pbprotocol::SaturatingIncrementUnsigned(
                implementation.outputReservationDeniedCount);
        }
        return FailureFrom<pbprotocol::OutputReservationDecision>(
            reservationResult.Error());
    }

    if (reservationResult.Value() ==
        pbprotocol::OutputReservationDecision::RequiresUserConfirmation)
    {
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.outputReservationRequiresConfirmationCount);
    }
    else
    {
        pbprotocol::SaturatingIncrementUnsigned(
            implementation.outputReservationAutoAcceptedCount);
    }
    return ReceiverResult<pbprotocol::OutputReservationDecision>::Success(
        reservationResult.Value());
}

[[nodiscard]] ReceiverResult<ReceiverControlAdmission>
HandleControlAdmission(
    detail::ReceiverIngressImplementation& implementation,
    pbprotocol::ControlRecordAdmission&& controlAdmission)
{
    if (const auto terminalError = FindTerminalSessionError(
            implementation,
            controlAdmission.sessionTag))
    {
        return ReceiverResult<ReceiverControlAdmission>::Failure(
            *terminalError);
    }

    ReceiverControlAdmission receiverAdmission;
    if (controlAdmission.recordType ==
        pbprotocol::ControlRecordType::SessionDescriptor)
    {
        const auto sessionResult = implementation.controlReceiver.
            GetSessionDescriptor(controlAdmission.sessionTag);
        if (!sessionResult)
        {
            return FailureFrom<ReceiverControlAdmission>(
                sessionResult.Error());
        }
        auto reservationResult = EvaluateOutputReservationCore(
            implementation,
            sessionResult.Value().originalFileSize);
        if (!reservationResult)
        {
            return ReceiverResult<ReceiverControlAdmission>::Failure(
                reservationResult.Error());
        }
        receiverAdmission.outputReservationDecision =
            reservationResult.Value();
    }
    else if (controlAdmission.recordType ==
        pbprotocol::ControlRecordType::SegmentDescriptor)
    {
        if (!controlAdmission.boundSegmentDescriptor)
        {
            return FailureFrom<ReceiverControlAdmission>(
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                    0});
        }
        auto orphanResult = ProcessOrphanBlocks(
            implementation,
            *controlAdmission.boundSegmentDescriptor);
        if (!orphanResult)
        {
            return ReceiverResult<ReceiverControlAdmission>::Failure(
                orphanResult.Error());
        }
        OrphanReplayOutcome orphanOutcome = std::move(orphanResult).Value();
        receiverAdmission.replayedOrphanBlocks = std::move(orphanOutcome.acceptedBlocks);
        receiverAdmission.completedSegment = std::move(orphanOutcome.completedSegment);
    }
    receiverAdmission.controlAdmission = std::move(controlAdmission);
    return ReceiverResult<ReceiverControlAdmission>::Success(
        std::move(receiverAdmission));
}

void AccumulateComponentTelemetryBeforeReset(
    detail::ReceiverIngressImplementation& implementation) noexcept
{
    implementation.priorControlRejectedCount =
        pbprotocol::SaturatingAddUnsigned(
            implementation.priorControlRejectedCount,
            implementation.controlReceiver.GetRejectedByResourcePolicyCount());
    implementation.priorOuterFecQuotaExceededCount =
        pbprotocol::SaturatingAddUnsigned(
            implementation.priorOuterFecQuotaExceededCount,
            implementation.outerFecResourceManager.GetQuotaExceededCount());
    implementation.priorOrphanAdmittedCount =
        pbprotocol::SaturatingAddUnsigned(
            implementation.priorOrphanAdmittedCount,
            implementation.orphanCache.GetAdmittedBlockCount());
    implementation.priorOrphanDroppedCount =
        pbprotocol::SaturatingAddUnsigned(
            implementation.priorOrphanDroppedCount,
            implementation.orphanCache.GetDroppedBlockCount());
    implementation.priorOrphanResourceExhaustedCount =
        pbprotocol::SaturatingAddUnsigned(
            implementation.priorOrphanResourceExhaustedCount,
            implementation.orphanCache.GetResourceExhaustedCount());
}

} // namespace

ReceiverIngress::ReceiverIngress(
    std::unique_ptr<detail::ReceiverIngressImplementation>
        implementation) noexcept
    : implementation_(std::move(implementation))
{
}

ReceiverIngress::ReceiverIngress(ReceiverIngress&&) noexcept = default;

ReceiverIngress& ReceiverIngress::operator=(ReceiverIngress&&) noexcept =
    default;

ReceiverIngress::~ReceiverIngress() = default;

ReceiverVerifiedSegment::ReceiverVerifiedSegment(
    pbprotocol::BoundSegmentDescriptor boundDescriptor,
    std::vector<std::byte> verifiedRawBytes) noexcept
    : boundSegmentDescriptor_(std::move(boundDescriptor)),
      rawBytes_(std::move(verifiedRawBytes))
{
}

ReceiverVerifiedSegment::ReceiverVerifiedSegment(
    ReceiverVerifiedSegment&& other) noexcept
    : boundSegmentDescriptor_(std::move(other.boundSegmentDescriptor_)),
      rawBytes_(std::move(other.rawBytes_)),
      valid_(std::exchange(other.valid_, false))
{
}

ReceiverVerifiedSegment& ReceiverVerifiedSegment::operator=(
    ReceiverVerifiedSegment&& other) noexcept
{
    if (this != &other)
    {
        boundSegmentDescriptor_ = std::move(other.boundSegmentDescriptor_);
        rawBytes_ = std::move(other.rawBytes_);
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

const pbprotocol::BoundSegmentDescriptor&
ReceiverVerifiedSegment::GetBoundSegmentDescriptor() const noexcept
{
    return boundSegmentDescriptor_;
}

std::span<const std::byte> ReceiverVerifiedSegment::GetRawBytes() const noexcept
{
    return rawBytes_;
}

ReceiverResult<ReceiverIngress> ReceiverIngress::Create(
    pbprotocol::ReceiverResourcePolicy resourcePolicy,
    const std::uint32_t expectedOuterBlockBytes)
{
    const pbprotocol::ProtocolStatus policyStatus =
        pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy);
    if (!policyStatus)
    {
        return FailureFrom<ReceiverIngress>(policyStatus.Error());
    }
    const auto profileStatus = ValidateProfileBlockBytes(
        resourcePolicy,
        expectedOuterBlockBytes);
    if (!profileStatus)
    {
        return ReceiverResult<ReceiverIngress>::Failure(
            profileStatus.Error());
    }

    const pbcompression::DecompressionLimits decompressionLimits =
        pbcompression::MakeDecompressionLimits(resourcePolicy);
    const pbcompression::CompressionStatus decompressionStatus =
        pbcompression::ValidateDecompressionLimits(decompressionLimits);
    if (!decompressionStatus)
    {
        return FailureFrom<ReceiverIngress>(decompressionStatus.Error());
    }

    auto controlResult = pbprotocol::ControlPlaneReceiver::Create(
        resourcePolicy);
    if (!controlResult)
    {
        return FailureFrom<ReceiverIngress>(controlResult.Error());
    }
    auto orphanResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    if (!orphanResult)
    {
        return FailureFrom<ReceiverIngress>(orphanResult.Error());
    }
    auto outerFecResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(resourcePolicy);
    if (!outerFecResult)
    {
        return FailureFrom<ReceiverIngress>(outerFecResult.Error());
    }

    try
    {
        auto implementation =
            std::make_unique<detail::ReceiverIngressImplementation>(
                std::move(resourcePolicy),
                expectedOuterBlockBytes,
                std::move(controlResult).Value(),
                std::move(orphanResult).Value(),
                std::move(outerFecResult).Value(),
                decompressionLimits);
        return ReceiverResult<ReceiverIngress>::Success(
            ReceiverIngress(std::move(implementation)));
    }
    catch (const std::bad_alloc&)
    {
        return FailureFrom<ReceiverIngress>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::ResourceExhausted,
            0});
    }
}

ReceiverResult<ReceiverControlAdmission> ReceiverIngress::ReceiveControlRecord(
    const std::span<const std::byte> recordBytes)
{
    if (!implementation_)
    {
        return FailureFrom<ReceiverControlAdmission>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    if (implementation_->globalTerminalError)
    {
        return ReceiverResult<ReceiverControlAdmission>::Failure(
            *implementation_->globalTerminalError);
    }

    auto controlResult = implementation_->controlReceiver.ReceiveControlRecord(
        recordBytes);
    if (!controlResult)
    {
        ClearLastTerminalControlSessionData(*implementation_);
        if (controlResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagCollision)
        {
            const auto viewResult = pbprotocol::ParseControlRecord(recordBytes);
            if (viewResult)
            {
                EraseActiveDecodersForSession(
                    *implementation_,
                    viewResult.Value().sessionTag);
                implementation_->orphanCache.ClearSession(
                    viewResult.Value().sessionTag);
                implementation_->terminalSessionErrors.erase(
                    viewResult.Value().sessionTag.value);
            }
        }
        ReceiverResult<ReceiverControlAdmission> result =
            FailureFrom<ReceiverControlAdmission>(controlResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }

    auto result = HandleControlAdmission(
        *implementation_,
        std::move(controlResult).Value());
    if (!result)
    {
        CountReturnedResourceFailure(*implementation_, result.Error());
    }
    return result;
}

ReceiverResult<ReceiverControlFragmentResult>
ReceiverIngress::ReceiveControlFragment(
    const std::span<const std::byte> fragmentBytes,
    const std::uint64_t observationOrdinal)
{
    if (!implementation_)
    {
        return FailureFrom<ReceiverControlFragmentResult>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    if (implementation_->globalTerminalError)
    {
        return ReceiverResult<ReceiverControlFragmentResult>::Failure(
            *implementation_->globalTerminalError);
    }

    auto fragmentResult = implementation_->controlReceiver.
        ReceiveControlFragment(fragmentBytes, observationOrdinal);
    if (!fragmentResult)
    {
        ClearLastTerminalControlSessionData(*implementation_);
        if (fragmentResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagCollision)
        {
            // A fragment envelope contains only ControlRecordId; the
            // offending SessionTag is available only inside the completed
            // reassembly. Clear all bounded Data-plane state rather than risk
            // retaining a decoder or orphan set for an ambiguous tag.
            ClearAllDataPlaneState(*implementation_);
        }
        ReceiverResult<ReceiverControlFragmentResult> result =
            FailureFrom<ReceiverControlFragmentResult>(fragmentResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }

    ReceiverControlFragmentResult receiverResult;
    receiverResult.disposition = fragmentResult.Value().disposition;
    if (fragmentResult.Value().admission)
    {
        auto admissionResult = HandleControlAdmission(
            *implementation_,
            std::move(*fragmentResult.Value().admission));
        if (!admissionResult)
        {
            CountReturnedResourceFailure(
                *implementation_,
                admissionResult.Error());
            return ReceiverResult<ReceiverControlFragmentResult>::Failure(
                admissionResult.Error());
        }
        receiverResult.admission = std::move(admissionResult).Value();
    }
    return ReceiverResult<ReceiverControlFragmentResult>::Success(
        std::move(receiverResult));
}

ReceiverResult<ReceiverDataAdmission> ReceiverIngress::ReceiveDataBlock(
    const ReceivedTransportBlock& transportBlock,
    const std::optional<std::uint64_t> observationOrdinal)
{
    if (!implementation_)
    {
        return FailureFrom<ReceiverDataAdmission>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
            0});
    }
    if (implementation_->globalTerminalError)
    {
        return ReceiverResult<ReceiverDataAdmission>::Failure(
            *implementation_->globalTerminalError);
    }

    const auto shapeResult = ValidateTransportBlockShape(
        transportBlock,
        implementation_->expectedOuterBlockBytes);
    if (!shapeResult)
    {
        return ReceiverResult<ReceiverDataAdmission>::Failure(
            shapeResult.Error());
    }
    if (implementation_->controlReceiver.IsTagAmbiguous(
            transportBlock.sessionTag))
    {
        return FailureFrom<ReceiverDataAdmission>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::SessionTagCollision,
            0});
    }
    if (const auto terminalError = FindTerminalSessionError(
            *implementation_,
            transportBlock.sessionTag))
    {
        return ReceiverResult<ReceiverDataAdmission>::Failure(*terminalError);
    }

    auto boundResult = implementation_->controlReceiver.
        GetBoundSegmentDescriptor(
            transportBlock.sessionTag,
            transportBlock.segmentOrdinal);
    if (!boundResult)
    {
        const pbprotocol::ProtocolErrorCode errorCode =
            boundResult.Error().code;
        if (errorCode == pbprotocol::ProtocolErrorCode::UnknownSession ||
            errorCode == pbprotocol::ProtocolErrorCode::UnknownSegment)
        {
            const std::uint64_t admittedBefore = implementation_->orphanCache.GetAdmittedBlockCount();
            const pbprotocol::ProtocolStatus orphanStatus =
                implementation_->orphanCache.Admit(
                    transportBlock.sessionTag,
                    transportBlock.segmentOrdinal,
                    transportBlock.outerBlockId,
                    transportBlock.declaredPayloadBytes,
                    transportBlock.paddedPayload,
                    observationOrdinal);
            if (!orphanStatus)
            {
                if (orphanStatus.Error().code ==
                    pbprotocol::ProtocolErrorCode::OrphanPayloadConflict)
                {
                    pbprotocol::SaturatingIncrementUnsigned(
                        implementation_->orphanConflictRejectionCount);
                }
                ReceiverResult<ReceiverDataAdmission> result =
                    FailureFrom<ReceiverDataAdmission>(orphanStatus.Error());
                CountReturnedResourceFailure(*implementation_, result.Error());
                return result;
            }
            const ReceiverOuterSymbolAdmission outerAdmission =
                implementation_->orphanCache.GetAdmittedBlockCount() > admittedBefore ?
                    ReceiverOuterSymbolAdmission::Unique : ReceiverOuterSymbolAdmission::IdenticalDuplicate;
            return ReceiverResult<ReceiverDataAdmission>::Success(
                ReceiverDataAdmission{
                    ReceiverDataDisposition::CachedOrphan,
                    std::nullopt,
                    outerAdmission});
        }

        ReceiverResult<ReceiverDataAdmission> result =
            FailureFrom<ReceiverDataAdmission>(boundResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }

    if (implementation_->orphanCache.HasCachedKey(
            transportBlock.sessionTag,
            transportBlock.segmentOrdinal))
    {
        // A descriptor may already be bound while a previous decoder-quota
        // refusal keeps pre-descriptor blocks cached. Compare/cache this new
        // observation before Drain so a full cache cannot hide a same-ID
        // conflict that arrives exactly when decoder capacity becomes free.
        const std::uint64_t admittedBefore = implementation_->orphanCache.GetAdmittedBlockCount();
        const pbprotocol::ProtocolStatus orphanStatus =
            implementation_->orphanCache.Admit(
                transportBlock.sessionTag,
                transportBlock.segmentOrdinal,
                transportBlock.outerBlockId,
                transportBlock.declaredPayloadBytes,
                transportBlock.paddedPayload,
                observationOrdinal);
        if (!orphanStatus)
        {
            if (orphanStatus.Error().code ==
                pbprotocol::ProtocolErrorCode::OrphanPayloadConflict)
            {
                pbprotocol::SaturatingIncrementUnsigned(
                    implementation_->orphanConflictRejectionCount);
                return ReceiverResult<ReceiverDataAdmission>::Failure(
                    LatchTerminalSessionError(
                        *implementation_,
                        transportBlock.sessionTag,
                        ReceiverError{orphanStatus.Error()}));
            }
            ReceiverResult<ReceiverDataAdmission> result =
                FailureFrom<ReceiverDataAdmission>(orphanStatus.Error());
            CountReturnedResourceFailure(*implementation_, result.Error());
            return result;
        }
        const ReceiverOuterSymbolAdmission outerAdmission =
            implementation_->orphanCache.GetAdmittedBlockCount() > admittedBefore ?
                ReceiverOuterSymbolAdmission::Unique : ReceiverOuterSymbolAdmission::IdenticalDuplicate;

        auto orphanResult = ProcessOrphanBlocks(
            *implementation_,
            boundResult.Value());
        if (!orphanResult)
        {
            ReceiverResult<ReceiverDataAdmission> result =
                ReceiverResult<ReceiverDataAdmission>::Failure(
                    orphanResult.Error());
            CountReturnedResourceFailure(*implementation_, result.Error());
            return result;
        }
        OrphanReplayOutcome orphanOutcome = std::move(orphanResult).Value();
        if (orphanOutcome.completedSegment)
        {
            return ReceiverResult<ReceiverDataAdmission>::Success(
                ReceiverDataAdmission{
                    ReceiverDataDisposition::EncodedSegmentReady,
                    std::move(orphanOutcome.completedSegment),
                    outerAdmission,
                    std::move(orphanOutcome.acceptedBlocks)});
        }
        if (implementation_->orphanCache.HasCachedKey(
                transportBlock.sessionTag,
                transportBlock.segmentOrdinal))
        {
            return ReceiverResult<ReceiverDataAdmission>::Success(
                ReceiverDataAdmission{
                    ReceiverDataDisposition::DeferredResourceBusy,
                    std::nullopt,
                    ReceiverOuterSymbolAdmission::DeferredResourceBusy});
        }
        return ReceiverResult<ReceiverDataAdmission>::Success(
            ReceiverDataAdmission{
                ReceiverDataDisposition::AcceptedNeedMore,
                std::nullopt,
                outerAdmission,
                std::move(orphanOutcome.acceptedBlocks)});
    }

    auto result = ProcessBoundDataBlock(
        *implementation_,
        boundResult.Value(),
        transportBlock);
    if (!result)
    {
        CountReturnedResourceFailure(*implementation_, result.Error());
    }
    return result;
}

ReceiverResult<std::vector<std::byte>> ReceiverIngress::ParseResumeState(
    const std::span<const std::byte> recordBytes)
{
    if (!implementation_)
    {
        return FailureFrom<std::vector<std::byte>>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    auto parseResult = pbprotocol::ParseResumeRecord(
        recordBytes,
        implementation_->resourcePolicy);
    if (!parseResult)
    {
        if (parseResult.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded)
        {
            pbprotocol::SaturatingIncrementUnsigned(
                implementation_->resumeQuotaRejectedCount);
        }
        ReceiverResult<std::vector<std::byte>> result =
            FailureFrom<std::vector<std::byte>>(parseResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }
    return ReceiverResult<std::vector<std::byte>>::Success(
        std::move(parseResult).Value());
}

ReceiverResult<pbprotocol::OutputReservationDecision>
ReceiverIngress::EvaluateOutputReservation(
    const std::uint64_t originalFileSize)
{
    if (!implementation_)
    {
        return FailureFrom<pbprotocol::OutputReservationDecision>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    auto result = EvaluateOutputReservationCore(
        *implementation_,
        originalFileSize);
    if (!result)
    {
        CountReturnedResourceFailure(*implementation_, result.Error());
    }
    return result;
}

ReceiverResult<std::vector<std::byte>> ReceiverIngress::DecompressSegment(
    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
    const std::span<const std::byte> encodedBytes)
{
    if (!implementation_)
    {
        return FailureFrom<std::vector<std::byte>>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    const pbprotocol::SegmentDescriptor& descriptor =
        boundSegmentDescriptor.GetDescriptor();
    if (const auto terminalError = FindTerminalSessionError(
            *implementation_,
            descriptor.sessionTag))
    {
        return ReceiverResult<std::vector<std::byte>>::Failure(*terminalError);
    }
    const auto currentBoundResult = implementation_->controlReceiver.
        GetBoundSegmentDescriptor(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!currentBoundResult ||
        currentBoundResult.Value() != boundSegmentDescriptor)
    {
        const pbprotocol::ProtocolError error = !currentBoundResult
            ? currentBoundResult.Error()
            : pbprotocol::ProtocolError{
                  pbprotocol::ProtocolErrorCode::DescriptorConflict,
                  0};
        return FailureFrom<std::vector<std::byte>>(error);
    }

    const auto actualEncodedSizeResult =
        pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(encodedBytes.size());
    std::optional<pbcompression::CompressionError> inputShapeError;
    if (!actualEncodedSizeResult ||
        descriptor.encodedSize >
            implementation_->decompressionLimits.maxInputBytes)
    {
        inputShapeError = pbcompression::CompressionError{
            pbcompression::CompressionErrorCode::InputLimitExceeded,
            descriptor.encodedSize};
    }
    else if (actualEncodedSizeResult.Value() >
             implementation_->decompressionLimits.maxInputBytes)
    {
        inputShapeError = pbcompression::CompressionError{
            pbcompression::CompressionErrorCode::InputLimitExceeded,
            actualEncodedSizeResult.Value()};
    }
    else if (actualEncodedSizeResult.Value() != descriptor.encodedSize)
    {
        inputShapeError = pbcompression::CompressionError{
            pbcompression::CompressionErrorCode::EncodedSizeMismatch,
            actualEncodedSizeResult.Value()};
    }
    if (inputShapeError)
    {
        CountDecompressionResourceFailure(
            *implementation_,
            inputShapeError->code);
        ReceiverResult<std::vector<std::byte>> result =
            FailureFrom<std::vector<std::byte>>(*inputShapeError);
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }

    const pbprotocol::EncodedDigest actualEncodedDigest{
        pbprotocol::ComputeBlake3Digest(encodedBytes)};
    if (actualEncodedDigest != descriptor.encodedDigest)
    {
        return FailureFrom<std::vector<std::byte>>(
            pbouterfec::OuterFecError{
                pbouterfec::OuterFecErrorCode::EncodedDigestMismatch,
                0});
    }

    auto decompressionResult = pbcompression::DecompressSegment(
        descriptor,
        encodedBytes,
        implementation_->decompressionLimits);
    if (!decompressionResult)
    {
        CountDecompressionResourceFailure(
            *implementation_,
            decompressionResult.Error().code);
        ReceiverResult<std::vector<std::byte>> result =
            FailureFrom<std::vector<std::byte>>(
                decompressionResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }

    const pbprotocol::RawDigest actualRawDigest{
        pbprotocol::ComputeBlake3Digest(decompressionResult.Value())};
    if (actualRawDigest != descriptor.rawDigest)
    {
        return FailureFrom<std::vector<std::byte>>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::DigestMismatch,
            0});
    }
    return ReceiverResult<std::vector<std::byte>>::Success(
        std::move(decompressionResult).Value());
}

ReceiverResult<ReceiverVerifiedSegment>
ReceiverIngress::VerifyRecoveredSegment(
    ReceiverCompletedSegment&& completedSegment)
{
    auto rawBytesResult = DecompressSegment(
        completedSegment.boundSegmentDescriptor,
        completedSegment.encodedBytes);
    if (!rawBytesResult)
    {
        return ReceiverResult<ReceiverVerifiedSegment>::Failure(
            rawBytesResult.Error());
    }

    return ReceiverResult<ReceiverVerifiedSegment>::Success(
        ReceiverVerifiedSegment(
            std::move(completedSegment.boundSegmentDescriptor),
            std::move(rawBytesResult).Value()));
}

ReceiverResult<ReceiverVerifiedSegment>
ReceiverIngress::VerifyResumedStoredSegment(
    const pbprotocol::ResumeCompletedSegmentRecord& completedRecord,
    std::vector<std::byte>&& storedRawBytes)
{
    if (!implementation_)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0});
    }
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(completedRecord.sessionId);
    if (const auto terminalError = FindTerminalSessionError(*implementation_, sessionTag))
    {
        return ReceiverResult<ReceiverVerifiedSegment>::Failure(*terminalError);
    }
    const auto sessionResult = implementation_->controlReceiver.GetSessionDescriptor(sessionTag);
    if (!sessionResult)
    {
        return FailureFrom<ReceiverVerifiedSegment>(sessionResult.Error());
    }
    if (sessionResult.Value().sessionId != completedRecord.sessionId)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::SessionMismatch, 0});
    }
    const auto actualRawSize = pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(storedRawBytes.size());
    if (!actualRawSize)
    {
        return FailureFrom<ReceiverVerifiedSegment>(actualRawSize.Error());
    }
    if (completedRecord.rawSize > implementation_->resourcePolicy.maxRawSegmentBytes ||
        actualRawSize.Value() > implementation_->resourcePolicy.maxRawSegmentBytes)
    {
        const pbprotocol::ProtocolError error{pbprotocol::ProtocolErrorCode::ResourceLimitExceeded, 0};
        CountReturnedResourceFailure(*implementation_, ReceiverError{error});
        return FailureFrom<ReceiverVerifiedSegment>(error);
    }
    if (completedRecord.segmentOrdinal >= sessionResult.Value().segmentCount)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange, 0});
    }
    const auto boundResult = implementation_->controlReceiver.GetBoundSegmentDescriptor(
        sessionTag, completedRecord.segmentOrdinal);
    if (!boundResult)
    {
        return FailureFrom<ReceiverVerifiedSegment>(boundResult.Error());
    }
    const pbprotocol::SegmentDescriptor& descriptor = boundResult.Value().GetDescriptor();
    if (completedRecord.rawOffset != descriptor.rawOffset ||
        completedRecord.rawSize != descriptor.rawSize ||
        completedRecord.rawDigest != descriptor.rawDigest)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::ResumeRecordConflict, 0});
    }
    if (actualRawSize.Value() != descriptor.rawSize)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidRecordSize, 0});
    }
    if (pbprotocol::RawDigest{pbprotocol::ComputeBlake3Digest(storedRawBytes)} != descriptor.rawDigest)
    {
        return FailureFrom<ReceiverVerifiedSegment>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::DigestMismatch, 0});
    }
    return ReceiverResult<ReceiverVerifiedSegment>::Success(
        ReceiverVerifiedSegment(boundResult.Value(), std::move(storedRawBytes)));
}

ReceiverResult<ReceiverSegmentCommitDisposition>
ReceiverIngress::CommitStoredSegment(
    ReceiverVerifiedSegment&& verifiedSegment)
{
    if (!implementation_)
    {
        return FailureFrom<ReceiverSegmentCommitDisposition>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    if (!verifiedSegment.valid_)
    {
        return FailureFrom<ReceiverSegmentCommitDisposition>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete,
                0});
    }

    const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor =
        verifiedSegment.GetBoundSegmentDescriptor();
    const pbprotocol::SegmentDescriptor& descriptor =
        boundSegmentDescriptor.GetDescriptor();
    if (const auto terminalError = FindTerminalSessionError(
            *implementation_,
            descriptor.sessionTag))
    {
        return ReceiverResult<ReceiverSegmentCommitDisposition>::Failure(
            *terminalError);
    }
    const auto currentBoundResult = implementation_->controlReceiver.
        GetBoundSegmentDescriptor(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!currentBoundResult ||
        currentBoundResult.Value() != boundSegmentDescriptor)
    {
        const pbprotocol::ProtocolError error = !currentBoundResult
            ? currentBoundResult.Error()
            : pbprotocol::ProtocolError{
                  pbprotocol::ProtocolErrorCode::DescriptorConflict,
                  0};
        return FailureFrom<ReceiverSegmentCommitDisposition>(error);
    }

    const auto completedResult = implementation_->controlReceiver.
        IsSegmentCompleted(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!completedResult)
    {
        return FailureFrom<ReceiverSegmentCommitDisposition>(
            completedResult.Error());
    }
    if (completedResult.Value())
    {
        implementation_->activeDecoders.erase(detail::ReceiverSegmentKey{
            descriptor.sessionTag.value,
            descriptor.segmentOrdinal});
        return ReceiverResult<ReceiverSegmentCommitDisposition>::Success(
            ReceiverSegmentCommitDisposition::AlreadyCommitted);
    }

    const pbprotocol::ProtocolStatus completionStatus =
        implementation_->controlReceiver.MarkSegmentCompleted(
            descriptor.sessionTag,
            descriptor.segmentOrdinal);
    if (!completionStatus)
    {
        return FailureFrom<ReceiverSegmentCommitDisposition>(
            completionStatus.Error());
    }
    implementation_->activeDecoders.erase(detail::ReceiverSegmentKey{
        descriptor.sessionTag.value,
        descriptor.segmentOrdinal});
    return ReceiverResult<ReceiverSegmentCommitDisposition>::Success(
        ReceiverSegmentCommitDisposition::Committed);
}

ReceiverResult<pbprotocol::FinalManifest>
ReceiverIngress::PrepareFinalization(
    const pbprotocol::SessionTag sessionTag)
{
    if (!implementation_)
    {
        return FailureFrom<pbprotocol::FinalManifest>(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
                0});
    }
    if (const auto terminalError = FindTerminalSessionError(
            *implementation_,
            sessionTag))
    {
        return ReceiverResult<pbprotocol::FinalManifest>::Failure(*terminalError);
    }
    const auto result = implementation_->controlReceiver.PrepareFinalization(
        sessionTag);
    if (!result)
    {
        return FailureFrom<pbprotocol::FinalManifest>(result.Error());
    }
    return ReceiverResult<pbprotocol::FinalManifest>::Success(
        std::move(result).Value());
}

ReceiverResult<bool> ReceiverIngress::RemoveSession(
    const pbprotocol::SessionId& sessionId)
{
    if (!implementation_)
    {
        return FailureFrom<bool>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
            0});
    }
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionId);
    const auto sessionResult = implementation_->controlReceiver.
        GetSessionDescriptor(sessionTag);
    if (!sessionResult)
    {
        if (sessionResult.Error().code ==
            pbprotocol::ProtocolErrorCode::UnknownSession)
        {
            return ReceiverResult<bool>::Success(false);
        }
        return FailureFrom<bool>(sessionResult.Error());
    }
    if (sessionResult.Value().sessionId != sessionId)
    {
        return ReceiverResult<bool>::Success(false);
    }

    EraseActiveDecodersForSession(*implementation_, sessionTag);
    implementation_->orphanCache.ClearSession(sessionTag);
    auto removeResult = implementation_->controlReceiver.RemoveSession(
        sessionId);
    if (!removeResult)
    {
        // Data-plane teardown has already happened. If the authoritative
        // Control registry cannot complete removal, stop all later admission
        // instead of allowing this partially torn-down Session to recreate a
        // decoder.
        implementation_->globalTerminalError = ReceiverError{
            removeResult.Error()};
        implementation_->terminalSessionErrors.clear();
        ReceiverResult<bool> result = FailureFrom<bool>(removeResult.Error());
        CountReturnedResourceFailure(*implementation_, result.Error());
        return result;
    }
    implementation_->terminalSessionErrors.erase(sessionTag.value);
    return ReceiverResult<bool>::Success(removeResult.Value());
}

ReceiverResult<bool> ReceiverIngress::ResetCaptureEpoch(
    const std::uint32_t expectedOuterBlockBytes)
{
    if (!implementation_)
    {
        return FailureFrom<bool>(pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
            0});
    }
    const auto profileStatus = ValidateProfileBlockBytes(
        implementation_->resourcePolicy,
        expectedOuterBlockBytes);
    if (!profileStatus)
    {
        return profileStatus;
    }

    auto controlResult = pbprotocol::ControlPlaneReceiver::Create(
        implementation_->resourcePolicy);
    if (!controlResult)
    {
        return FailureFrom<bool>(controlResult.Error());
    }
    auto orphanResult = pbprotocol::OrphanTransportBlockCache::Create(
        implementation_->resourcePolicy);
    if (!orphanResult)
    {
        return FailureFrom<bool>(orphanResult.Error());
    }
    auto outerFecResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(
            implementation_->resourcePolicy);
    if (!outerFecResult)
    {
        return FailureFrom<bool>(outerFecResult.Error());
    }

    AccumulateComponentTelemetryBeforeReset(*implementation_);
    implementation_->activeDecoders.clear();
    implementation_->orphanCache = std::move(orphanResult).Value();
    implementation_->terminalSessionErrors.clear();
    implementation_->globalTerminalError.reset();
    implementation_->controlReceiver = std::move(controlResult).Value();
    implementation_->outerFecResourceManager =
        std::move(outerFecResult).Value();
    implementation_->expectedOuterBlockBytes = expectedOuterBlockBytes;
    return ReceiverResult<bool>::Success(true);
}

ReceiverResourceTelemetrySnapshot ReceiverIngress::GetTelemetry() const
{
    if (!implementation_)
    {
        return {};
    }
    ReceiverResourceTelemetrySnapshot telemetry;
    telemetry.totalResourcePolicyRejectedCount =
        implementation_->totalResourcePolicyRejectedCount;
    telemetry.controlRejectedByResourcePolicyCount =
        pbprotocol::SaturatingAddUnsigned(
        implementation_->priorControlRejectedCount,
        implementation_->controlReceiver.
            GetRejectedByResourcePolicyCount());
    telemetry.activeControlReassemblyCount =
        implementation_->controlReceiver.ActiveControlReassemblyCount();
    telemetry.controlReassemblyBytesInUse =
        implementation_->controlReceiver.ControlReassemblyBytesInUse();
    telemetry.outerFecQuotaExceededCount =
        pbprotocol::SaturatingAddUnsigned(
        implementation_->priorOuterFecQuotaExceededCount,
        implementation_->outerFecResourceManager.GetQuotaExceededCount());
    telemetry.deferredResourceBusyCount = implementation_->deferredResourceBusyCount;
    telemetry.orphanAdmittedBlockCount =
        pbprotocol::SaturatingAddUnsigned(
        implementation_->priorOrphanAdmittedCount,
        implementation_->orphanCache.GetAdmittedBlockCount());
    telemetry.orphanDroppedByQuotaCount =
        pbprotocol::SaturatingAddUnsigned(
        implementation_->priorOrphanDroppedCount,
        implementation_->orphanCache.GetDroppedBlockCount());
    telemetry.orphanResourceExhaustedCount =
        pbprotocol::SaturatingAddUnsigned(
        implementation_->priorOrphanResourceExhaustedCount,
        implementation_->orphanCache.GetResourceExhaustedCount());
    telemetry.orphanConflictRejectionCount =
        implementation_->orphanConflictRejectionCount;
    telemetry.orphanCachedBlockCount =
        implementation_->orphanCache.GetCachedBlockCount();
    telemetry.orphanCachedBytes = implementation_->orphanCache.GetCachedBytes();
    telemetry.resumeQuotaRejectedCount =
        implementation_->resumeQuotaRejectedCount;
    telemetry.decompressionResourceRejectedCount =
        implementation_->decompressionResourceRejectedCount;
    telemetry.decompressionInputQuotaRejectedCount =
        implementation_->decompressionInputQuotaRejectedCount;
    telemetry.decompressionOutputQuotaRejectedCount =
        implementation_->decompressionOutputQuotaRejectedCount;
    telemetry.decompressionWindowQuotaRejectedCount =
        implementation_->decompressionWindowQuotaRejectedCount;
    telemetry.decompressionAllocationFailureCount =
        implementation_->decompressionAllocationFailureCount;
    telemetry.outputReservationDeniedCount =
        implementation_->outputReservationDeniedCount;
    telemetry.outputReservationRequiresConfirmationCount =
        implementation_->outputReservationRequiresConfirmationCount;
    telemetry.outputReservationAutoAcceptedCount =
        implementation_->outputReservationAutoAcceptedCount;
    telemetry.activeSessionCount =
        implementation_->controlReceiver.ActiveSessionCount();
    telemetry.reservedDescriptorStateBytes =
        implementation_->controlReceiver.ReservedDescriptorStateBytes();
    telemetry.activeOuterFecDecoderCount =
        implementation_->outerFecResourceManager.GetActiveDecoderCount();
    telemetry.reservedOuterFecDecoderBytes =
        implementation_->outerFecResourceManager.GetReservedDecoderBytes();
    return telemetry;
}

} // namespace pbreceiver
