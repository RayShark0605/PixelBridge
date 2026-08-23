#include "pbcompression/segment_decompression.h"

#include "compression_internal.h"
#include "pbprotocol/checked_integer.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace pbcompression {

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
    if (limits.maxOutputBytes
        == std::numeric_limits<std::uint64_t>::max())
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            limits.maxOutputBytes);
    }
    return CompressionStatus::Success();
}

SegmentDecompressor::SegmentDecompressor() noexcept = default;

SegmentDecompressor::SegmentDecompressor(SegmentDecompressor&& other) noexcept
    : limits_(other.limits_)
    , expectedRawSize_(other.expectedRawSize_)
    , decompressContext_(other.decompressContext_)
    , inputBuffer_(std::move(other.inputBuffer_))
    , inputPosition_(other.inputPosition_)
    , outputBuffer_(std::move(other.outputBuffer_))
    , producedBytes_(other.producedBytes_)
    , frameComplete_(other.frameComplete_)
    , frameFinished_(other.frameFinished_)
    , frameContentSizeChecked_(other.frameContentSizeChecked_)
    , frameHeaderBytes_(other.frameHeaderBytes_)
    , moved_(false)
    , terminalError_(other.terminalError_)
{
    other.decompressContext_ = nullptr;
    other.inputBuffer_.clear();
    other.inputPosition_ = 0;
    other.outputBuffer_.clear();
    other.producedBytes_ = 0;
    other.frameComplete_ = false;
    other.frameHeaderBytes_ = 0;
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
    const std::uint64_t expectedRawSize)
{
    const CompressionStatus limitsStatus =
        ValidateDecompressionLimits(limits);
    if (!limitsStatus)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            limitsStatus.Error().code, limitsStatus.Error().detail);
    }

    // Policy runs before allocation (33.1): the requested output must fit
    // the configured budget before any buffer exists.
    if (expectedRawSize > limits.maxOutputBytes)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::OutputLimitExceeded, expectedRawSize);
    }

    ZSTD_DStream* decompressStream = ZSTD_createDStream();
    if (decompressStream == nullptr)
    {
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    auto releaseAndFail = [decompressStream](const std::size_t functionResult)
    {
        const CompressionError mappedError =
            detail::MapZstdFunctionResult(functionResult);
        ZSTD_freeDStream(decompressStream);
        return CompressionResult<SegmentDecompressor>::Failure(
            mappedError.code, mappedError.detail);
    };

    // The DStream layout keeps the DCtx as its first member; zstd itself
    // relies on that cast internally, so the public layout contract holds.
    const std::size_t windowLimitResult = ZSTD_DCtx_setParameter(
        static_cast<ZSTD_DCtx*>(decompressStream),
        ZSTD_d_windowLogMax,
        static_cast<int>(limits.maxWindowLog));
    if (ZSTD_isError(windowLimitResult))
    {
        return releaseAndFail(windowLimitResult);
    }

    // One probe byte beyond the expected size (when the budget allows) so
    // an overrunning frame writes into the probe instead of escaping the
    // bound; expectedRawSize <= maxOutputBytes < UINT64_MAX, so the +1
    // cannot overflow.
    const std::uint64_t outputBytes =
        expectedRawSize + 1 <= limits.maxOutputBytes
            ? expectedRawSize + 1
            : expectedRawSize;

    SegmentDecompressor decompressor;
    try
    {
        decompressor.outputBuffer_.resize(
            static_cast<std::size_t>(outputBytes));
    }
    catch (const std::bad_alloc&)
    {
        ZSTD_freeDStream(decompressStream);
        return CompressionResult<SegmentDecompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    decompressor.limits_ = limits;
    decompressor.expectedRawSize_ = expectedRawSize;
    decompressor.decompressContext_ =
        static_cast<void*>(decompressStream);
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

CompressionStatus SegmentDecompressor::CheckFrameContentSizePrefix()
{
    if (frameContentSizeChecked_ || inputBuffer_.empty())
    {
        return CompressionStatus::Success();
    }
    if (inputBuffer_.size() < ZSTD_FRAMEHEADERSIZE_MAX)
    {
        // A header parse below the maximum header size is not
        // authoritative; keep accumulating until the full header is in.
        return CompressionStatus::Success();
    }
    frameContentSizeChecked_ = true;

    // The buffer holds at least ZSTD_FRAMEHEADERSIZE_MAX bytes, so a
    // parse success is authoritative for the exact header and content
    // sizes; a parse failure is corruption, not "read more input".
    ZSTD_FrameHeader frameHeader{};
    const std::size_t headerParseResult = ZSTD_getFrameHeader(
        &frameHeader, inputBuffer_.data(), inputBuffer_.size());
    if (ZSTD_isError(headerParseResult))
    {
        const CompressionError mappedError =
            detail::MapZstdFunctionResult(headerParseResult);
        terminalError_ = mappedError;
        return CompressionStatus::Failure(
            mappedError.code, mappedError.detail);
    }
    frameHeaderBytes_ = frameHeader.headerSize;

    if (frameHeader.frameContentSize == ZSTD_CONTENTSIZE_ERROR)
    {
        // Defensive: a valid header parse should not yield this
        // sentinel, but keep the old fail-closed branch anyway.
        const CompressionError corruptedError{
            CompressionErrorCode::CorruptedFrame,
            static_cast<std::uint64_t>(ZSTD_error_prefix_unknown)};
        terminalError_ = corruptedError;
        return CompressionStatus::Failure(
            corruptedError.code, corruptedError.detail);
    }
    if (frameHeader.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        // No FCS field in this frame header; the strict produced-size
        // check at Finish() still applies.
        return CompressionStatus::Success();
    }
    if (frameHeader.frameContentSize > limits_.maxOutputBytes)
    {
        const CompressionError limitError{
            CompressionErrorCode::OutputLimitExceeded,
            frameHeader.frameContentSize};
        terminalError_ = limitError;
        return CompressionStatus::Failure(
            limitError.code, limitError.detail);
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
    if (frameComplete_)
    {
        // Input after a complete frame can only be trailing bytes; feeding
        // it to the decoder would start a second frame header.
        const CompressionError trailingError{
            CompressionErrorCode::TrailingInput,
            static_cast<std::uint64_t>(input.size())};
        terminalError_ = trailingError;
        return CompressionStatus::Failure(
            trailingError.code, trailingError.detail);
    }

    const std::size_t appendPosition = inputBuffer_.size();
    const auto appendSizeResult = pbprotocol::CheckedAddSize(
        appendPosition, static_cast<std::size_t>(input.size()));
    if (!appendSizeResult)
    {
        const CompressionError limitError{
            CompressionErrorCode::OutputLimitExceeded, 0};
        terminalError_ = limitError;
        return CompressionStatus::Failure(
            limitError.code, limitError.detail);
    }

    try
    {
        inputBuffer_.resize(appendSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        const CompressionError allocationError{
            CompressionErrorCode::AllocationFailure, 0};
        terminalError_ = allocationError;
        return CompressionStatus::Failure(
            allocationError.code, allocationError.detail);
    }

    std::copy(
        input.begin(), input.end(),
        inputBuffer_.data() + appendPosition);

    const CompressionStatus prefixStatus = CheckFrameContentSizePrefix();
    if (!prefixStatus)
    {
        return prefixStatus;
    }

    ZSTD_DStream* decompressStream =
        static_cast<ZSTD_DStream*>(decompressContext_);

    while (inputPosition_ < inputBuffer_.size() && !frameComplete_)
    {
        const std::size_t remainingBytes =
            inputBuffer_.size() - inputPosition_;
        std::size_t feedBytes = remainingBytes;
        if (frameHeaderBytes_ != 0
            && inputPosition_ < frameHeaderBytes_)
        {
            // Offer only the frame header first: a whole frame in one
            // call would let the streaming decoder take a single-pass
            // shortcut that skips the window-limit check, so the header
            // must be consumed in its own call to force that check.
            feedBytes = std::min(
                remainingBytes, frameHeaderBytes_ - inputPosition_);
        }
        const std::size_t freeBytes =
            outputBuffer_.size() - producedBytes_;
        ZSTD_inBuffer inputChunk{};
        inputChunk.src = inputBuffer_.data() + inputPosition_;
        inputChunk.size = feedBytes;
        inputChunk.pos = 0;
        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.data() + producedBytes_;
        outputChunk.size = freeBytes;
        outputChunk.pos = 0;

        const std::size_t decodeResult = ZSTD_decompressStream(
            decompressStream, &outputChunk, &inputChunk);
        if (ZSTD_isError(decodeResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(decodeResult);
            terminalError_ = mappedError;
            return CompressionStatus::Failure(
                mappedError.code, mappedError.detail);
        }

        const auto producedBytesResult = pbprotocol::CheckedAddUint64(
            producedBytes_,
            static_cast<std::uint64_t>(outputChunk.pos));
        if (!producedBytesResult)
        {
            const CompressionError limitError{
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_};
            terminalError_ = limitError;
            return CompressionStatus::Failure(
                limitError.code, limitError.detail);
        }
        producedBytes_ = producedBytesResult.Value();
        inputPosition_ += inputChunk.pos;

        // Order matters: an overrun of the declared expected size is a
        // limit violation even if the frame happens to complete in the
        // same call.
        if (producedBytes_ > expectedRawSize_)
        {
            const CompressionError limitError{
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_};
            terminalError_ = limitError;
            return CompressionStatus::Failure(
                limitError.code, limitError.detail);
        }
        if (decodeResult == 0)
        {
            // 0 means the frame is fully decoded (checksum included) and
            // the output has been fully flushed.
            frameComplete_ = true;
            break;
        }
        if (inputChunk.pos == 0 && outputChunk.pos == 0)
        {
            // Defensive: with remaining input zstd must make progress or
            // report an error, so zero progress here means the bounded
            // output is the blocker, or the frame is incomplete with a
            // full buffer that can never fill further.
            const CompressionErrorCode stalledCode =
                freeBytes == 0
                    ? CompressionErrorCode::OutputLimitExceeded
                    : CompressionErrorCode::IncompleteFrame;
            const CompressionError stalledError{stalledCode, 0};
            terminalError_ = stalledError;
            return CompressionStatus::Failure(
                stalledError.code, stalledError.detail);
        }
    }

    if (frameComplete_ && inputPosition_ != inputBuffer_.size())
    {
        // The single-pass decoder shortcut consumes exactly the frame
        // bytes; anything left over is trailing input.
        const CompressionError trailingError{
            CompressionErrorCode::TrailingInput,
            static_cast<std::uint64_t>(
                inputBuffer_.size() - inputPosition_)};
        terminalError_ = trailingError;
        return CompressionStatus::Failure(
            trailingError.code, trailingError.detail);
    }

    // Exhausted input with an incomplete frame is a legal streaming state;
    // Finish() decides whether it is a truncation.
    return CompressionStatus::Success();
}

CompressionResult<std::vector<std::byte>> SegmentDecompressor::Finish()
{
    const CompressionStatus stateStatus = CheckWritableState();
    if (!stateStatus)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            stateStatus.Error().code, stateStatus.Error().detail);
    }

    const CompressionStatus prefixStatus = CheckFrameContentSizePrefix();
    if (!prefixStatus)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            prefixStatus.Error().code, prefixStatus.Error().detail);
    }

    ZSTD_DStream* decompressStream =
        static_cast<ZSTD_DStream*>(decompressContext_);

    while (!frameComplete_)
    {
        const std::size_t freeBytes =
            outputBuffer_.size() - producedBytes_;
        ZSTD_inBuffer emptyInput{};
        emptyInput.src = nullptr;
        emptyInput.size = 0;
        emptyInput.pos = 0;
        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.data() + producedBytes_;
        outputChunk.size = freeBytes;
        outputChunk.pos = 0;

        const std::size_t decodeResult = ZSTD_decompressStream(
            decompressStream, &outputChunk, &emptyInput);
        if (ZSTD_isError(decodeResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(decodeResult);
            terminalError_ = mappedError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                mappedError.code, mappedError.detail);
        }

        const auto producedBytesResult = pbprotocol::CheckedAddUint64(
            producedBytes_,
            static_cast<std::uint64_t>(outputChunk.pos));
        if (!producedBytesResult)
        {
            const CompressionError limitError{
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_};
            terminalError_ = limitError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitError.code, limitError.detail);
        }
        producedBytes_ = producedBytesResult.Value();

        if (producedBytes_ > expectedRawSize_)
        {
            const CompressionError limitError{
                CompressionErrorCode::OutputLimitExceeded,
                producedBytes_};
            terminalError_ = limitError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                limitError.code, limitError.detail);
        }

        if (decodeResult == 0)
        {
            frameComplete_ = true;
            break;
        }

        if (outputChunk.pos == 0)
        {
            // Empty input with an incomplete frame: the decoder is waiting
            // for bytes that never arrive (or the bounded output cannot
            // absorb the rest of the frame).
            const CompressionErrorCode stalledCode =
                freeBytes == 0
                    ? CompressionErrorCode::OutputLimitExceeded
                    : CompressionErrorCode::IncompleteFrame;
            const CompressionError stalledError{stalledCode, 0};
            terminalError_ = stalledError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                stalledError.code, stalledError.detail);
        }
    }

    if (inputPosition_ != inputBuffer_.size())
    {
        const CompressionError trailingError{
            CompressionErrorCode::TrailingInput,
            static_cast<std::uint64_t>(
                inputBuffer_.size() - inputPosition_)};
        terminalError_ = trailingError;
        return CompressionResult<std::vector<std::byte>>::Failure(
            trailingError.code, trailingError.detail);
    }

    if (producedBytes_ != expectedRawSize_)
    {
        const CompressionError sizeError{
            CompressionErrorCode::RawSizeMismatch, producedBytes_};
        terminalError_ = sizeError;
        return CompressionResult<std::vector<std::byte>>::Failure(
            sizeError.code, sizeError.detail);
    }

    frameFinished_ = true;
    outputBuffer_.resize(static_cast<std::size_t>(producedBytes_));
    return CompressionResult<std::vector<std::byte>>::Success(
        std::move(outputBuffer_));
}

CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::CompressionCodec codec,
    const std::span<const std::byte> encodedBytes,
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

    if (expectedRawSize > limits.maxOutputBytes)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            CompressionErrorCode::OutputLimitExceeded, expectedRawSize);
    }

    switch (codec)
    {
        case pbprotocol::CompressionCodec::Raw:
        {
            if (static_cast<std::uint64_t>(encodedBytes.size())
                != expectedRawSize)
            {
                // On this platform size_t and uint64 have equal width, so
                // the comparison is exact.
                return CompressionResult<std::vector<std::byte>>::Failure(
                    CompressionErrorCode::RawSizeMismatch,
                    static_cast<std::uint64_t>(encodedBytes.size()));
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
            return CompressionResult<std::vector<std::byte>>::Success(
                std::move(rawBytes));
        }
        case pbprotocol::CompressionCodec::Zstandard:
        {
            auto decompressorResult = SegmentDecompressor::Create(
                limits, expectedRawSize);
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
            // An unknown codec value is a descriptor conflict: fail closed
            // with the raw value visible for diagnosis.
            return CompressionResult<std::vector<std::byte>>::Failure(
                CompressionErrorCode::ZstdError,
                static_cast<std::uint64_t>(codec));
    }
}

} // namespace pbcompression