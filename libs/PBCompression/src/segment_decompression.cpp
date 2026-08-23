#include "pbcompression/segment_decompression.h"

#include "compression_internal.h"
#include "pbprotocol/checked_integer.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

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

namespace pbcompression {
namespace {

static_assert(
    kZstdFrameHeaderMaximumBytes == ZSTD_FRAMEHEADERSIZE_MAX,
    "PBCompression's bounded header staging must track zstd's maximum frame header size");

[[nodiscard]] bool FitsByteVector(const std::uint64_t byteCount) noexcept
{
    const auto sizeResult = pbprotocol::CheckedUint64ToSize(byteCount);
    if (!sizeResult)
    {
        return false;
    }

    const std::vector<std::byte> emptyBytes;
    return sizeResult.Value() <= emptyBytes.max_size();
}

[[nodiscard]] std::optional<std::uint64_t> NarrowSizeToUint64(
    const std::size_t byteCount) noexcept
{
    const auto sizeResult =
        pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(byteCount);
    if (!sizeResult)
    {
        return std::nullopt;
    }
    return sizeResult.Value();
}

[[nodiscard]] CompressionStatus MakeFailure(
    std::optional<CompressionError>& terminalError,
    const CompressionErrorCode code,
    const std::uint64_t detail = 0) noexcept
{
    terminalError = CompressionError{code, detail};
    return CompressionStatus::Failure(code, detail);
}

[[nodiscard]] CompressionError MapZstdDecompressionResult(
    const std::size_t functionResult) noexcept
{
    const ZSTD_ErrorCode errorCode = ZSTD_getErrorCode(functionResult);
    if (errorCode == ZSTD_error_frameParameter_windowTooLarge)
    {
        return CompressionError{
            CompressionErrorCode::WindowLimitExceeded,
            static_cast<std::uint64_t>(errorCode)};
    }
    return detail::MapZstdFunctionResult(functionResult);
}

} // namespace

DecompressionLimits MakeDecompressionLimits(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint32_t maxWindowLog) noexcept
{
    DecompressionLimits limits;
    limits.maxOutputBytes = resourcePolicy.maxRawSegmentBytes;
    limits.maxWindowLog = maxWindowLog;
    limits.maxInputBytes = resourcePolicy.maxEncodedSegmentBytes;
    return limits;
}

CompressionStatus ValidateDecompressionLimits(
    const DecompressionLimits& limits) noexcept
{
    // Fail closed on out-of-range window logs: silently clamping them would
    // change which frames are accepted without the caller knowing.
    const std::uint32_t windowLogMinimum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MIN);
    const std::uint32_t windowLogMaximum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MAX);
    if (limits.maxWindowLog < windowLogMinimum
        || limits.maxWindowLog > windowLogMaximum)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxWindowLog,
            static_cast<std::uint64_t>(limits.maxWindowLog));
    }
    if (!FitsByteVector(limits.maxOutputBytes))
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            limits.maxOutputBytes);
    }
    if (!FitsByteVector(limits.maxInputBytes))
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxInputBytes,
            limits.maxInputBytes);
    }
    return CompressionStatus::Success();
}

SegmentDecompressor::SegmentDecompressor() noexcept = default;

SegmentDecompressor::SegmentDecompressor(SegmentDecompressor&& other) noexcept
    : limits_(other.limits_)
    , expectedEncodedSize_(other.expectedEncodedSize_)
    , expectedRawSize_(other.expectedRawSize_)
    , receivedEncodedBytes_(other.receivedEncodedBytes_)
    , decompressContext_(other.decompressContext_)
    , frameHeaderBuffer_(other.frameHeaderBuffer_)
    , frameHeaderBufferedBytes_(other.frameHeaderBufferedBytes_)
    , outputBuffer_(std::move(other.outputBuffer_))
    , producedBytes_(other.producedBytes_)
    , frameComplete_(other.frameComplete_)
    , frameFinished_(other.frameFinished_)
    , frameHeaderParsed_(other.frameHeaderParsed_)
    , frameContentSizeKnown_(other.frameContentSizeKnown_)
    , moved_(false)
    , terminalError_(other.terminalError_)
{
    other.decompressContext_ = nullptr;
    other.expectedEncodedSize_ = 0;
    other.expectedRawSize_ = 0;
    other.receivedEncodedBytes_ = 0;
    other.frameHeaderBufferedBytes_ = 0;
    other.outputBuffer_.clear();
    other.producedBytes_ = 0;
    other.frameComplete_ = false;
    other.frameFinished_ = false;
    other.frameHeaderParsed_ = false;
    other.frameContentSizeKnown_ = false;
    other.moved_ = true;
    other.terminalError_ = CompressionError{
        CompressionErrorCode::InvalidState, 0};
}

SegmentDecompressor::~SegmentDecompressor()
{
    if (decompressContext_ != nullptr)
    {
        ZSTD_freeDStream(static_cast<ZSTD_DStream*>(decompressContext_));
        decompressContext_ = nullptr;
    }
}

CompressionResult<SegmentDecompressor> SegmentDecompressor::Create(
    const DecompressionLimits& limits,
    const std::uint64_t expectedEncodedSize,
    const std::uint64_t expectedRawSize)
{
    const CompressionStatus limitsStatus =
        ValidateDecompressionLimits(limits);
    if (!limitsStatus)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            limitsStatus.Error().code, limitsStatus.Error().detail);
    }

    if (expectedEncodedSize == 0
        || !FitsByteVector(expectedEncodedSize))
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::InvalidExpectedEncodedSize,
            expectedEncodedSize);
    }
    if (expectedEncodedSize > limits.maxInputBytes)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::InputLimitExceeded,
            expectedEncodedSize);
    }
    if (expectedRawSize > limits.maxOutputBytes)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::OutputLimitExceeded, expectedRawSize);
    }

    std::uint64_t outputBytes = expectedRawSize;
    if (expectedRawSize < limits.maxOutputBytes)
    {
        const auto probeSizeResult = pbprotocol::CheckedAddUint64(
            expectedRawSize, 1);
        if (!probeSizeResult
            || probeSizeResult.Value() > limits.maxOutputBytes)
        {
            return CompressionResult<SegmentDecompressor>::Failure(
                CompressionErrorCode::InvalidExpectedRawSize,
                expectedRawSize);
        }
        outputBytes = probeSizeResult.Value();
    }

    const auto outputSizeResult =
        pbprotocol::CheckedUint64ToSize(outputBytes);
    if (!outputSizeResult || !FitsByteVector(outputBytes))
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize, outputBytes);
    }

    SegmentDecompressor decompressor;
    try
    {
        decompressor.outputBuffer_.resize(outputSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize, outputBytes);
    }

    using DecompressStreamPointer =
        std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)>;
    DecompressStreamPointer decompressStream(
        ZSTD_createDStream(), &ZSTD_freeDStream);
    if (decompressStream == nullptr)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    // ZSTD_DStream is zstd's streaming alias for ZSTD_DCtx; keep zstd types
    // out of the public header while configuring the documented DCtx limit.
    const std::size_t windowLimitResult = ZSTD_DCtx_setParameter(
        static_cast<ZSTD_DCtx*>(decompressStream.get()),
        ZSTD_d_windowLogMax,
        static_cast<int>(limits.maxWindowLog));
    if (ZSTD_isError(windowLimitResult))
    {
        const CompressionError mappedError =
            detail::MapZstdFunctionResult(windowLimitResult);
        return CompressionResult<SegmentDecompressor>::Failure(
            mappedError.code, mappedError.detail);
    }

    decompressor.limits_ = limits;
    decompressor.expectedEncodedSize_ = expectedEncodedSize;
    decompressor.expectedRawSize_ = expectedRawSize;
    decompressor.decompressContext_ =
        static_cast<void*>(decompressStream.release());
    return CompressionResult<SegmentDecompressor>::Success(
        std::move(decompressor));
}

CompressionStatus SegmentDecompressor::CheckWritableState() const noexcept
{
    if (moved_ || frameFinished_)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidState, 0);
    }
    if (terminalError_.has_value())
    {
        return CompressionStatus::Failure(
            terminalError_->code, terminalError_->detail);
    }
    return CompressionStatus::Success();
}

CompressionStatus SegmentDecompressor::DecodeInput(
    const std::span<const std::byte> input)
{
    if (input.empty())
    {
        return CompressionStatus::Success();
    }
    const std::optional<std::uint64_t> inputBytesResult =
        NarrowSizeToUint64(input.size());
    if (!inputBytesResult.has_value())
    {
        return MakeFailure(
            terminalError_, CompressionErrorCode::InputLimitExceeded);
    }
    if (frameComplete_)
    {
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::TrailingInput,
            inputBytesResult.value());
    }

    ZSTD_DStream* const decompressStream =
        static_cast<ZSTD_DStream*>(decompressContext_);
    std::size_t inputPosition = 0;
    while (inputPosition < input.size() && !frameComplete_)
    {
        const auto producedSizeResult =
            pbprotocol::CheckedUint64ToSize(producedBytes_);
        if (!producedSizeResult
            || producedSizeResult.Value() > outputBuffer_.size())
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
        }

        const std::size_t producedSize = producedSizeResult.Value();
        const std::size_t freeBytes = outputBuffer_.size() - producedSize;
        ZSTD_inBuffer inputChunk{};
        inputChunk.src = input.data() + inputPosition;
        inputChunk.size = input.size() - inputPosition;
        inputChunk.pos = 0;
        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.empty()
            ? nullptr
            : static_cast<void*>(outputBuffer_.data() + producedSize);
        outputChunk.size = freeBytes;
        outputChunk.pos = 0;

        const std::size_t decodeResult = ZSTD_decompressStream(
            decompressStream, &outputChunk, &inputChunk);
        if (ZSTD_isError(decodeResult))
        {
            const CompressionError mappedError =
                MapZstdDecompressionResult(decodeResult);
            return MakeFailure(
                terminalError_, mappedError.code, mappedError.detail);
        }

        const std::optional<std::uint64_t> outputBytesResult =
            NarrowSizeToUint64(outputChunk.pos);
        if (!outputBytesResult.has_value())
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
        }
        const auto producedBytesResult = pbprotocol::CheckedAddUint64(
            producedBytes_,
            outputBytesResult.value());
        if (!producedBytesResult)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
        }
        producedBytes_ = producedBytesResult.Value();
        const auto inputPositionResult = pbprotocol::CheckedAddSize(
            inputPosition, inputChunk.pos);
        if (!inputPositionResult
            || inputPositionResult.Value() > input.size())
        {
            return MakeFailure(
                terminalError_, CompressionErrorCode::ZstdError);
        }
        inputPosition = inputPositionResult.Value();

        if (producedBytes_ > expectedRawSize_)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
        }
        if (decodeResult == 0)
        {
            frameComplete_ = true;
            break;
        }
        if (inputChunk.pos == 0 && outputChunk.pos == 0)
        {
            const CompressionErrorCode stalledCode = freeBytes == 0
                ? CompressionErrorCode::OutputLimitExceeded
                : CompressionErrorCode::IncompleteFrame;
            return MakeFailure(terminalError_, stalledCode, 0);
        }
    }

    if (frameComplete_ && inputPosition != input.size())
    {
        const std::optional<std::uint64_t> trailingBytesResult =
            NarrowSizeToUint64(input.size() - inputPosition);
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::TrailingInput,
            trailingBytesResult.value_or(
                std::numeric_limits<std::uint64_t>::max()));
    }
    return CompressionStatus::Success();
}

CompressionStatus SegmentDecompressor::ParseAndDecodeFrameHeader(
    std::span<const std::byte>& remainingInput)
{
    while (!frameHeaderParsed_ && !remainingInput.empty())
    {
        const std::size_t availableHeaderBytes =
            frameHeaderBuffer_.size() - frameHeaderBufferedBytes_;
        const std::size_t copyBytes = std::min(
            availableHeaderBytes, remainingInput.size());
        std::copy_n(
            remainingInput.begin(),
            copyBytes,
            frameHeaderBuffer_.begin()
                + static_cast<std::ptrdiff_t>(frameHeaderBufferedBytes_));
        const auto bufferedBytesResult = pbprotocol::CheckedAddSize(
            frameHeaderBufferedBytes_, copyBytes);
        if (!bufferedBytesResult
            || bufferedBytesResult.Value() > frameHeaderBuffer_.size())
        {
            return MakeFailure(
                terminalError_, CompressionErrorCode::CorruptedFrame);
        }
        frameHeaderBufferedBytes_ = bufferedBytesResult.Value();
        remainingInput = remainingInput.subspan(copyBytes);

        ZSTD_FrameHeader frameHeader{};
        const std::size_t headerParseResult = ZSTD_getFrameHeader(
            &frameHeader,
            frameHeaderBuffer_.data(),
            frameHeaderBufferedBytes_);
        if (ZSTD_isError(headerParseResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(headerParseResult);
            return MakeFailure(
                terminalError_, mappedError.code, mappedError.detail);
        }
        if (headerParseResult != 0)
        {
            if (frameHeaderBufferedBytes_ == frameHeaderBuffer_.size())
            {
                return MakeFailure(
                    terminalError_,
                    CompressionErrorCode::CorruptedFrame,
                    static_cast<std::uint64_t>(headerParseResult));
            }
            continue;
        }

        if (frameHeader.frameType != ZSTD_frame)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::CorruptedFrame,
                static_cast<std::uint64_t>(frameHeader.frameType));
        }
        if (frameHeader.headerSize == 0
            || frameHeader.headerSize > frameHeaderBufferedBytes_)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::CorruptedFrame,
                static_cast<std::uint64_t>(frameHeader.headerSize));
        }
        if (frameHeader.frameContentSize == ZSTD_CONTENTSIZE_ERROR)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::CorruptedFrame,
                static_cast<std::uint64_t>(ZSTD_error_prefix_unknown));
        }
        if (frameHeader.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN
            && frameHeader.frameContentSize != expectedRawSize_)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::RawSizeMismatch,
                frameHeader.frameContentSize);
        }
        frameContentSizeKnown_ =
            frameHeader.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN;

        const std::uint64_t maximumWindowBytes =
            std::uint64_t{1} << limits_.maxWindowLog;
        if (frameHeader.windowSize > maximumWindowBytes)
        {
            return MakeFailure(
                terminalError_,
                CompressionErrorCode::WindowLimitExceeded,
                frameHeader.windowSize);
        }

        frameHeaderParsed_ = true;
        const std::span<const std::byte> exactHeader(
            frameHeaderBuffer_.data(), frameHeader.headerSize);
        const CompressionStatus headerStatus = DecodeInput(exactHeader);
        if (!headerStatus)
        {
            return headerStatus;
        }

        const std::size_t stagedPayloadBytes =
            frameHeaderBufferedBytes_ - frameHeader.headerSize;
        if (stagedPayloadBytes != 0)
        {
            const std::span<const std::byte> stagedPayload(
                frameHeaderBuffer_.data() + frameHeader.headerSize,
                stagedPayloadBytes);
            const CompressionStatus payloadStatus =
                DecodeInput(stagedPayload);
            if (!payloadStatus)
            {
                return payloadStatus;
            }
        }
    }
    return CompressionStatus::Success();
}

CompressionStatus SegmentDecompressor::Update(
    const std::span<const std::byte> input)
{
    const CompressionStatus stateStatus = CheckWritableState();
    if (!stateStatus)
    {
        return stateStatus;
    }
    if (input.empty())
    {
        return CompressionStatus::Success();
    }
    const std::optional<std::uint64_t> inputBytesResult =
        NarrowSizeToUint64(input.size());
    if (!inputBytesResult.has_value())
    {
        return MakeFailure(
            terminalError_, CompressionErrorCode::InputLimitExceeded);
    }
    if (frameComplete_)
    {
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::TrailingInput,
            inputBytesResult.value());
    }

    const auto receivedBytesResult = pbprotocol::CheckedAddUint64(
        receivedEncodedBytes_,
        inputBytesResult.value());
    if (!receivedBytesResult)
    {
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::InputLimitExceeded,
            receivedEncodedBytes_);
    }
    if (receivedBytesResult.Value() > limits_.maxInputBytes)
    {
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::InputLimitExceeded,
            receivedBytesResult.Value());
    }
    if (receivedBytesResult.Value() > expectedEncodedSize_)
    {
        return MakeFailure(
            terminalError_,
            CompressionErrorCode::EncodedSizeMismatch,
            receivedBytesResult.Value());
    }
    receivedEncodedBytes_ = receivedBytesResult.Value();

    std::span<const std::byte> remainingInput = input;
    if (!frameHeaderParsed_)
    {
        const CompressionStatus headerStatus =
            ParseAndDecodeFrameHeader(remainingInput);
        if (!headerStatus)
        {
            return headerStatus;
        }
    }
    if (!frameHeaderParsed_)
    {
        return CompressionStatus::Success();
    }
    return DecodeInput(remainingInput);
}

CompressionResult<std::vector<std::byte>> SegmentDecompressor::Finish()
{
    const CompressionStatus stateStatus = CheckWritableState();
    if (!stateStatus)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            stateStatus.Error().code, stateStatus.Error().detail);
    }
    if (receivedEncodedBytes_ != expectedEncodedSize_)
    {
        const CompressionStatus mismatchStatus = MakeFailure(
            terminalError_,
            CompressionErrorCode::EncodedSizeMismatch,
            receivedEncodedBytes_);
        return CompressionResult<std::vector<std::byte>>::Failure(
            mismatchStatus.Error().code, mismatchStatus.Error().detail);
    }
    if (!frameHeaderParsed_)
    {
        const CompressionStatus incompleteStatus = MakeFailure(
            terminalError_, CompressionErrorCode::IncompleteFrame, 0);
        return CompressionResult<std::vector<std::byte>>::Failure(
            incompleteStatus.Error().code, incompleteStatus.Error().detail);
    }

    ZSTD_DStream* const decompressStream =
        static_cast<ZSTD_DStream*>(decompressContext_);
    while (!frameComplete_)
    {
        const auto producedSizeResult =
            pbprotocol::CheckedUint64ToSize(producedBytes_);
        if (!producedSizeResult
            || producedSizeResult.Value() > outputBuffer_.size())
        {
            const CompressionStatus limitStatus = MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitStatus.Error().code, limitStatus.Error().detail);
        }

        const std::size_t producedSize = producedSizeResult.Value();
        const std::size_t freeBytes = outputBuffer_.size() - producedSize;
        ZSTD_inBuffer emptyInput{};
        emptyInput.src = nullptr;
        emptyInput.size = 0;
        emptyInput.pos = 0;
        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.empty()
            ? nullptr
            : static_cast<void*>(outputBuffer_.data() + producedSize);
        outputChunk.size = freeBytes;
        outputChunk.pos = 0;

        const std::size_t decodeResult = ZSTD_decompressStream(
            decompressStream, &outputChunk, &emptyInput);
        if (ZSTD_isError(decodeResult))
        {
            const CompressionError mappedError =
                MapZstdDecompressionResult(decodeResult);
            const CompressionStatus decodeStatus = MakeFailure(
                terminalError_, mappedError.code, mappedError.detail);
            return CompressionResult<std::vector<std::byte>>::Failure(
                decodeStatus.Error().code, decodeStatus.Error().detail);
        }

        const std::optional<std::uint64_t> outputBytesResult =
            NarrowSizeToUint64(outputChunk.pos);
        if (!outputBytesResult.has_value())
        {
            const CompressionStatus limitStatus = MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitStatus.Error().code, limitStatus.Error().detail);
        }
        const auto producedBytesResult = pbprotocol::CheckedAddUint64(
            producedBytes_,
            outputBytesResult.value());
        if (!producedBytesResult)
        {
            const CompressionStatus limitStatus = MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitStatus.Error().code, limitStatus.Error().detail);
        }
        producedBytes_ = producedBytesResult.Value();
        if (producedBytes_ > expectedRawSize_)
        {
            const CompressionStatus limitStatus = MakeFailure(
                terminalError_,
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_);
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitStatus.Error().code, limitStatus.Error().detail);
        }
        if (decodeResult == 0)
        {
            frameComplete_ = true;
            break;
        }
        if (outputChunk.pos == 0)
        {
            const bool knownSizeIsFullyProduced =
                frameContentSizeKnown_
                && producedBytes_ == expectedRawSize_;
            const CompressionErrorCode stalledCode =
                freeBytes == 0 && !knownSizeIsFullyProduced
                    ? CompressionErrorCode::OutputLimitExceeded
                    : CompressionErrorCode::IncompleteFrame;
            const CompressionStatus stalledStatus = MakeFailure(
                terminalError_, stalledCode, 0);
            return CompressionResult<std::vector<std::byte>>::Failure(
                stalledStatus.Error().code, stalledStatus.Error().detail);
        }
    }

    if (producedBytes_ != expectedRawSize_)
    {
        const CompressionStatus mismatchStatus = MakeFailure(
            terminalError_,
            CompressionErrorCode::RawSizeMismatch,
            producedBytes_);
        return CompressionResult<std::vector<std::byte>>::Failure(
            mismatchStatus.Error().code, mismatchStatus.Error().detail);
    }

    const auto finalSizeResult =
        pbprotocol::CheckedUint64ToSize(producedBytes_);
    if (!finalSizeResult
        || finalSizeResult.Value() > outputBuffer_.size())
    {
        const CompressionStatus limitStatus = MakeFailure(
            terminalError_,
            CompressionErrorCode::OutputLimitExceeded,
            producedBytes_);
        return CompressionResult<std::vector<std::byte>>::Failure(
            limitStatus.Error().code, limitStatus.Error().detail);
    }
    try
    {
        outputBuffer_.resize(finalSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        const CompressionStatus allocationStatus = MakeFailure(
            terminalError_, CompressionErrorCode::AllocationFailure, 0);
        return CompressionResult<std::vector<std::byte>>::Failure(
            allocationStatus.Error().code,
            allocationStatus.Error().detail);
    }
    catch (const std::length_error&)
    {
        const CompressionStatus sizeStatus = MakeFailure(
            terminalError_,
            CompressionErrorCode::InvalidExpectedRawSize,
            producedBytes_);
        return CompressionResult<std::vector<std::byte>>::Failure(
            sizeStatus.Error().code, sizeStatus.Error().detail);
    }
    frameFinished_ = true;
    return CompressionResult<std::vector<std::byte>>::Success(
        std::move(outputBuffer_));
}

CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::CompressionCodec codec,
    const std::span<const std::byte> encodedBytes,
    const std::uint64_t expectedEncodedSize,
    const std::uint64_t expectedRawSize,
    const DecompressionLimits& limits)
{
    const CompressionStatus limitsStatus =
        ValidateDecompressionLimits(limits);
    if (!limitsStatus)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            limitsStatus.Error().code, limitsStatus.Error().detail);
    }

    if (!FitsByteVector(expectedEncodedSize))
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::InvalidExpectedEncodedSize,
            expectedEncodedSize);
    }
    if (expectedEncodedSize > limits.maxInputBytes)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::InputLimitExceeded,
            expectedEncodedSize);
    }
    if (expectedRawSize > limits.maxOutputBytes)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::OutputLimitExceeded, expectedRawSize);
    }

    const auto actualSizeResult =
        pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(encodedBytes.size());
    if (!actualSizeResult)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::InputLimitExceeded,
            expectedEncodedSize);
    }
    if (actualSizeResult.Value() > limits.maxInputBytes)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::InputLimitExceeded,
            actualSizeResult.Value());
    }
    if (actualSizeResult.Value() != expectedEncodedSize)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::EncodedSizeMismatch,
            actualSizeResult.Value());
    }

    switch (codec)
    {
        case pbprotocol::CompressionCodec::Raw:
        {
            if (expectedEncodedSize != expectedRawSize)
            {
                return CompressionResult<std::vector<std::byte>>::Failure(
                    CompressionErrorCode::RawSizeMismatch,
                    expectedEncodedSize);
            }

            std::vector<std::byte> rawBytes;
            try
            {
                rawBytes.assign(encodedBytes.begin(), encodedBytes.end());
            }
            catch (const std::bad_alloc&)
            {
                return CompressionResult<std::vector<std::byte>>::Failure(
                    CompressionErrorCode::AllocationFailure);
            }
            catch (const std::length_error&)
            {
                return CompressionResult<std::vector<std::byte>>::Failure(
                    CompressionErrorCode::InvalidExpectedRawSize,
                    expectedRawSize);
            }
            return CompressionResult<std::vector<std::byte>>::Success(
                std::move(rawBytes));
        }
        case pbprotocol::CompressionCodec::Zstandard:
        {
            auto decompressorResult = SegmentDecompressor::Create(
                limits, expectedEncodedSize, expectedRawSize);
            if (!decompressorResult)
            {
                return CompressionResult<std::vector<std::byte>>::Failure(
                    decompressorResult.Error().code,
                    decompressorResult.Error().detail);
            }
            SegmentDecompressor decompressor =
                std::move(decompressorResult).Value();

            const CompressionStatus updateStatus =
                decompressor.Update(encodedBytes);
            if (!updateStatus)
            {
                return CompressionResult<std::vector<std::byte>>::Failure(
                    updateStatus.Error().code,
                    updateStatus.Error().detail);
            }
            return decompressor.Finish();
        }
        default:
            return CompressionResult<std::vector<std::byte>>::Failure(
                CompressionErrorCode::UnsupportedCompressionCodec,
                static_cast<std::uint64_t>(codec));
    }
}

CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::SegmentDescriptor& descriptor,
    const std::span<const std::byte> encodedBytes,
    const DecompressionLimits& limits)
{
    return DecompressSegment(
        descriptor.compressionCodec,
        encodedBytes,
        descriptor.encodedSize,
        descriptor.rawSize,
        limits);
}

} // namespace pbcompression
