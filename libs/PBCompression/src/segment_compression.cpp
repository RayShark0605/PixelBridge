#include "pbcompression/segment_compression.h"

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

CompressionError detail::MapZstdFunctionResult(const std::size_t functionResult) noexcept
{
    const ZSTD_ErrorCode errorCode = ZSTD_getErrorCode(functionResult);
    switch (errorCode)
    {
        case ZSTD_error_dstSize_tooSmall:
        case ZSTD_error_frameParameter_windowTooLarge:
        case ZSTD_error_noForwardProgress_destFull:
            return CompressionError{
                CompressionErrorCode::OutputLimitExceeded,
                static_cast<std::uint64_t>(errorCode)};
        case ZSTD_error_srcSize_wrong:
            return CompressionError{
                CompressionErrorCode::RawSizeMismatch,
                static_cast<std::uint64_t>(errorCode)};
        case ZSTD_error_corruption_detected:
        case ZSTD_error_checksum_wrong:
            return CompressionError{
                CompressionErrorCode::CorruptedFrame,
                static_cast<std::uint64_t>(errorCode)};
        case ZSTD_error_memory_allocation:
            return CompressionError{
                CompressionErrorCode::AllocationFailure,
                static_cast<std::uint64_t>(errorCode)};
        case ZSTD_error_noForwardProgress_inputEmpty:
            return CompressionError{
                CompressionErrorCode::IncompleteFrame,
                static_cast<std::uint64_t>(errorCode)};
        default:
            // Not in the fixed table: keep the raw code visible and fail
            // closed.
            return CompressionError{
                CompressionErrorCode::ZstdError,
                static_cast<std::uint64_t>(errorCode)};
    }
}

pbprotocol::CompressionCodec ChooseSegmentCodec(
    const std::uint64_t compressedBytes,
    const std::uint64_t rawBytes,
    const std::uint64_t framingMarginBytes) noexcept
{
    const auto totalBytesResult = pbprotocol::CheckedAddUint64(
        compressedBytes, framingMarginBytes);
    if (!totalBytesResult)
    {
        // Overflow means the encoded payload cannot be represented inside
        // any 64-bit budget: keep the segment Raw (fail closed).
        return pbprotocol::CompressionCodec::Raw;
    }
    if (totalBytesResult.Value() >= rawBytes)
    {
        return pbprotocol::CompressionCodec::Raw;
    }
    return pbprotocol::CompressionCodec::Zstandard;
}

SegmentCompressor::SegmentCompressor() noexcept = default;

SegmentCompressor::SegmentCompressor(SegmentCompressor&& other) noexcept
    : settings_(other.settings_)
    , expectedRawBytes_(other.expectedRawBytes_)
    , fedBytes_(other.fedBytes_)
    , compressContext_(other.compressContext_)
    , outputBuffer_(std::move(other.outputBuffer_))
    , outputSize_(other.outputSize_)
    , frameFinished_(other.frameFinished_)
    , moved_(false)
    , terminalError_(other.terminalError_)
{
    other.compressContext_ = nullptr;
    other.outputBuffer_.clear();
    other.outputSize_ = 0;
    other.moved_ = true;
    other.terminalError_ = CompressionError{
        CompressionErrorCode::InvalidState, 0};
}

SegmentCompressor::~SegmentCompressor()
{
    if (compressContext_ != nullptr)
    {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(compressContext_));
        compressContext_ = nullptr;
    }
}

CompressionResult<SegmentCompressor> SegmentCompressor::Create(
    const CompressionSettings& settings,
    const std::uint64_t expectedRawBytes)
{
    const int maxCompressionLevel = ZSTD_maxCLevel();
    if (settings.compressionLevel < 1
        || settings.compressionLevel > maxCompressionLevel)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidCompressionLevel,
            static_cast<std::uint64_t>(settings.compressionLevel));
    }

    // Fail closed on out-of-range window logs: silently clamping them would
    // change the encoded byte stream without the caller knowing.
    const std::uint32_t windowLogMinimum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MIN);
    const std::uint32_t windowLogMaximum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MAX);
    if (settings.maxWindowLog < windowLogMinimum
        || settings.maxWindowLog > windowLogMaximum)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidMaxWindowLog,
            static_cast<std::uint64_t>(settings.maxWindowLog));
    }

    if (settings.maxOutputBytes < kMinFrameBytes
        || settings.maxOutputBytes
            >= std::numeric_limits<std::uint64_t>::max())
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            settings.maxOutputBytes);
    }

    ZSTD_CCtx* compressContext = ZSTD_createCCtx();
    if (compressContext == nullptr)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    auto releaseAndFail = [compressContext](const std::size_t functionResult)
    {
        const CompressionError mappedError =
            detail::MapZstdFunctionResult(functionResult);
        ZSTD_freeCCtx(compressContext);
        return CompressionResult<SegmentCompressor>::Failure(
            mappedError.code, mappedError.detail);
    };

    const std::size_t levelResult = ZSTD_CCtx_setParameter(
        compressContext,
        ZSTD_c_compressionLevel,
        settings.compressionLevel);
    if (ZSTD_isError(levelResult))
    {
        return releaseAndFail(levelResult);
    }

    const std::size_t windowResult = ZSTD_CCtx_setParameter(
        compressContext,
        ZSTD_c_windowLog,
        static_cast<int>(settings.maxWindowLog));
    if (ZSTD_isError(windowResult))
    {
        return releaseAndFail(windowResult);
    }

    // A 32-bit frame checksum makes single-bit corruption fail
    // deterministically instead of decoding into plausible garbage.
    const std::size_t checksumResult = ZSTD_CCtx_setParameter(
        compressContext, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(checksumResult))
    {
        return releaseAndFail(checksumResult);
    }

    std::uint64_t initialBytes;
    if (expectedRawBytes != 0)
    {
        const auto pledgedSizeResult =
            pbprotocol::CheckedUint64ToSize(expectedRawBytes);
        if (!pledgedSizeResult)
        {
            ZSTD_freeCCtx(compressContext);
            return CompressionResult<SegmentCompressor>::Failure(
                CompressionErrorCode::InvalidExpectedRawSize,
                expectedRawBytes);
        }

        const std::size_t pledgedResult = ZSTD_CCtx_setPledgedSrcSize(
            compressContext, expectedRawBytes);
        if (ZSTD_isError(pledgedResult))
        {
            return releaseAndFail(pledgedResult);
        }

        const std::size_t boundResult =
            ZSTD_compressBound(pledgedSizeResult.Value());
        if (ZSTD_isError(boundResult))
        {
            return releaseAndFail(boundResult);
        }
        initialBytes = std::min(
            static_cast<std::uint64_t>(boundResult),
            settings.maxOutputBytes);
    }
    else
    {
        initialBytes = std::min(
            kInitialStreamBytes, settings.maxOutputBytes);
    }
    initialBytes = std::max(initialBytes, kMinFrameBytes);

    SegmentCompressor compressor;
    try
    {
        compressor.outputBuffer_.resize(
            static_cast<std::size_t>(initialBytes));
    }
    catch (const std::bad_alloc&)
    {
        ZSTD_freeCCtx(compressContext);
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    compressor.settings_ = settings;
    compressor.expectedRawBytes_ = expectedRawBytes;
    compressor.compressContext_ = static_cast<void*>(compressContext);
    return CompressionResult<SegmentCompressor>::Success(
        std::move(compressor));
}

CompressionStatus SegmentCompressor::CheckWritableState() const noexcept
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

CompressionStatus SegmentCompressor::GrowOutputBuffer(
    const std::uint64_t minimumFreeBytes)
{
    const std::uint64_t currentBytes = outputBuffer_.size();
    const std::uint64_t maximumBytes = settings_.maxOutputBytes;

    // currentBytes is always <= maximumBytes, so the subtraction below
    // cannot underflow.
    std::uint64_t desiredBytes;
    if (currentBytes > maximumBytes - currentBytes)
    {
        desiredBytes = maximumBytes;
    }
    else
    {
        desiredBytes = currentBytes * 2;
    }

    const auto minimumBytesResult = pbprotocol::CheckedAddUint64(
        outputSize_, minimumFreeBytes);
    if (!minimumBytesResult)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::OutputLimitExceeded, outputSize_);
    }
    if (minimumBytesResult.Value() > desiredBytes)
    {
        desiredBytes = minimumBytesResult.Value();
    }

    if (desiredBytes > maximumBytes || desiredBytes <= currentBytes)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::OutputLimitExceeded, maximumBytes);
    }

    try
    {
        outputBuffer_.resize(static_cast<std::size_t>(desiredBytes));
    }
    catch (const std::bad_alloc&)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::AllocationFailure, 0);
    }
    return CompressionStatus::Success();
}

CompressionStatus SegmentCompressor::Update(
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

    ZSTD_CCtx* compressContext =
        static_cast<ZSTD_CCtx*>(compressContext_);

    ZSTD_inBuffer inputBuffer{};
    inputBuffer.src = input.data();
    inputBuffer.size = static_cast<std::size_t>(input.size());
    inputBuffer.pos = 0;

    while (inputBuffer.pos < inputBuffer.size)
    {
        const std::size_t freeBytes =
            outputBuffer_.size() - outputSize_;
        if (freeBytes == 0)
        {
            const CompressionStatus growStatus = GrowOutputBuffer(1);
            if (!growStatus)
            {
                terminalError_ = growStatus.Error();
                return growStatus;
            }
        }

        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.data() + outputSize_;
        outputChunk.size = outputBuffer_.size() - outputSize_;
        outputChunk.pos = 0;

        const std::size_t consumedBefore = inputBuffer.pos;
        const std::size_t streamResult = ZSTD_compressStream2(
            compressContext, &outputChunk, &inputBuffer, ZSTD_e_continue);
        if (ZSTD_isError(streamResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(streamResult);
            terminalError_ = mappedError;
            return CompressionStatus::Failure(
                mappedError.code, mappedError.detail);
        }

        const std::uint64_t consumedBytes =
            static_cast<std::uint64_t>(inputBuffer.pos - consumedBefore);
        if (consumedBytes == 0 && outputChunk.pos == 0)
        {
            // Defensive: with both remaining input and free output space
            // zstd must consume input, emit output, or report an error.
            // Stalling here means the bounded output cannot absorb the
            // frame, so fail instead of looping forever.
            const CompressionError stalledError{
                CompressionErrorCode::OutputLimitExceeded, 0};
            terminalError_ = stalledError;
            return CompressionStatus::Failure(
                CompressionErrorCode::OutputLimitExceeded, 0);
        }

        const auto fedBytesResult =
            pbprotocol::CheckedAddUint64(fedBytes_, consumedBytes);
        if (!fedBytesResult)
        {
            const CompressionError fedError{
                CompressionErrorCode::InvalidExpectedRawSize,
                fedBytes_};
            terminalError_ = fedError;
            return CompressionStatus::Failure(
                CompressionErrorCode::InvalidExpectedRawSize, fedBytes_);
        }
        fedBytes_ = fedBytesResult.Value();

        outputSize_ += outputChunk.pos;
    }

    return CompressionStatus::Success();
}

CompressionResult<std::vector<std::byte>> SegmentCompressor::Finish()
{
    const CompressionStatus stateStatus = CheckWritableState();
    if (!stateStatus)
    {
        return CompressionResult<std::vector<std::byte>>::Failure(
            stateStatus.Error().code, stateStatus.Error().detail);
    }

    // A pledged source size mismatch is deterministic: the frame header
    // carries the pledged size, so the decoder would reject it anyway.
    if (expectedRawBytes_ != 0 && fedBytes_ != expectedRawBytes_)
    {
        const CompressionError pledgedError{
            CompressionErrorCode::RawSizeMismatch, fedBytes_};
        terminalError_ = pledgedError;
        return CompressionResult<std::vector<std::byte>>::Failure(
            pledgedError.code, pledgedError.detail);
    }

    ZSTD_CCtx* compressContext =
        static_cast<ZSTD_CCtx*>(compressContext_);

    for (;;)
    {
        const std::size_t freeBytes =
            outputBuffer_.size() - outputSize_;
        if (freeBytes == 0)
        {
            const CompressionStatus growStatus = GrowOutputBuffer(1);
            if (!growStatus)
            {
                terminalError_ = growStatus.Error();
                return CompressionResult<std::vector<std::byte>>::Failure(
                    growStatus.Error().code, growStatus.Error().detail);
            }
        }

        ZSTD_inBuffer emptyInput{};
        emptyInput.src = nullptr;
        emptyInput.size = 0;
        emptyInput.pos = 0;

        ZSTD_outBuffer outputChunk{};
        outputChunk.dst = outputBuffer_.data() + outputSize_;
        outputChunk.size = outputBuffer_.size() - outputSize_;
        outputChunk.pos = 0;

        const std::size_t flushResult = ZSTD_compressStream2(
            compressContext, &outputChunk, &emptyInput, ZSTD_e_end);
        if (ZSTD_isError(flushResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(flushResult);
            terminalError_ = mappedError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                mappedError.code, mappedError.detail);
        }

        outputSize_ += outputChunk.pos;

        if (flushResult == 0)
        {
            // ZSTD_e_end returns 0 exactly when the frame is complete and
            // the output buffer has been fully flushed.
            frameFinished_ = true;
            outputBuffer_.resize(outputSize_);
            return CompressionResult<std::vector<std::byte>>::Success(
                std::move(outputBuffer_));
        }

        if (outputChunk.pos == 0)
        {
            // Free space was just grown (or already existed) yet nothing
            // was produced: a stalled end flush cannot be fixed by growing
            // again, so fail instead of looping.
            const CompressionError stalledError{
                CompressionErrorCode::OutputLimitExceeded, 0};
            terminalError_ = stalledError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                CompressionErrorCode::OutputLimitExceeded, 0);
        }
    }
}

CompressionResult<EncodedSegment> CompressSegment(
    const std::span<const std::byte> input,
    const CompressionSettings& settings)
{
    if (input.empty())
    {
        // Empty segments stay Raw with zero bytes, which also satisfies
        // the descriptor rule encodedSize == rawSize for Raw segments.
        return CompressionResult<EncodedSegment>::Success(EncodedSegment{});
    }

    auto compressorResult = SegmentCompressor::Create(
        settings, static_cast<std::uint64_t>(input.size()));
    if (!compressorResult)
    {
        return CompressionResult<EncodedSegment>::Failure(
            compressorResult.Error().code, compressorResult.Error().detail);
    }
    SegmentCompressor compressor = std::move(compressorResult).Value();

    const CompressionStatus updateStatus = compressor.Update(input);
    if (!updateStatus)
    {
        return CompressionResult<EncodedSegment>::Failure(
            updateStatus.Error().code, updateStatus.Error().detail);
    }

    auto frameResult = compressor.Finish();
    if (!frameResult)
    {
        return CompressionResult<EncodedSegment>::Failure(
            frameResult.Error().code, frameResult.Error().detail);
    }

    const pbprotocol::CompressionCodec codec = ChooseSegmentCodec(
        static_cast<std::uint64_t>(frameResult.Value().size()),
        static_cast<std::uint64_t>(input.size()),
        settings.framingMarginBytes);

    EncodedSegment encodedSegment;
    encodedSegment.codec = codec;
    if (codec == pbprotocol::CompressionCodec::Zstandard)
    {
        encodedSegment.bytes = std::move(frameResult).Value();
    }
    else
    {
        // Guarded like every other allocation in this library so the
        // CompressionResult contract is not broken by an escaped throw.
        try
        {
            encodedSegment.bytes.assign(input.begin(), input.end());
        }
        catch (const std::bad_alloc&)
        {
            return CompressionResult<EncodedSegment>::Failure(
                CompressionErrorCode::AllocationFailure);
        }
    }
    return CompressionResult<EncodedSegment>::Success(
        std::move(encodedSegment));
}

} // namespace pbcompression