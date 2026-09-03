#include "pbcompression/segment_compression.h"

#include "compression_internal.h"
#include "pbprotocol/checked_integer.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pbcompression {

std::string GetZstandardBaselineIdentity()
{
    return std::string("zstd-") + ZSTD_versionString();
}

namespace {

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

[[nodiscard]] CompressionStatus ValidateCompressionSettings(
    const CompressionSettings& settings) noexcept
{
    const int maxCompressionLevel = ZSTD_maxCLevel();
    if (settings.compressionLevel < 1
        || settings.compressionLevel > maxCompressionLevel)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidCompressionLevel,
            static_cast<std::uint64_t>(settings.compressionLevel));
    }

    const std::uint32_t windowLogMinimum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MIN);
    const std::uint32_t windowLogMaximum =
        static_cast<std::uint32_t>(ZSTD_WINDOWLOG_MAX);
    if (settings.maxWindowLog < windowLogMinimum
        || settings.maxWindowLog > windowLogMaximum)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxWindowLog,
            static_cast<std::uint64_t>(settings.maxWindowLog));
    }

    if (settings.maxOutputBytes < kMinFrameBytes
        || !FitsByteVector(settings.maxOutputBytes))
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            settings.maxOutputBytes);
    }
    return CompressionStatus::Success();
}

[[nodiscard]] CompressionResult<EncodedSegment> CopyRawSegment(
    const std::span<const std::byte> input,
    const CompressionSettings& settings)
{
    const std::optional<std::uint64_t> rawBytesResult =
        NarrowSizeToUint64(input.size());
    if (!rawBytesResult.has_value())
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize);
    }
    const std::uint64_t rawBytes = rawBytesResult.value();
    if (rawBytes > settings.maxOutputBytes)
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::OutputLimitExceeded, rawBytes);
    }

    EncodedSegment encodedSegment;
    try
    {
        encodedSegment.bytes.assign(input.begin(), input.end());
    }
    catch (const std::bad_alloc&)
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize, rawBytes);
    }
    return CompressionResult<EncodedSegment>::Success(
        std::move(encodedSegment));
}

} // namespace

CompressionError detail::MapZstdFunctionResult(const std::size_t functionResult) noexcept
{
    const ZSTD_ErrorCode errorCode = ZSTD_getErrorCode(functionResult);
    switch (errorCode)
    {
        case ZSTD_error_dstSize_tooSmall:
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
        case ZSTD_error_prefix_unknown:
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
    const CompressionStatus settingsStatus =
        ValidateCompressionSettings(settings);
    if (!settingsStatus)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            settingsStatus.Error().code,
            settingsStatus.Error().detail);
    }

    if (expectedRawBytes != 0
        && !FitsByteVector(expectedRawBytes))
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize,
            expectedRawBytes);
    }

    std::uint64_t initialBytes;
    std::optional<std::size_t> pledgedSize;
    if (expectedRawBytes != 0)
    {
        const auto pledgedSizeResult =
            pbprotocol::CheckedUint64ToSize(expectedRawBytes);
        if (!pledgedSizeResult)
        {
            return CompressionResult<SegmentCompressor>::Failure(
                CompressionErrorCode::InvalidExpectedRawSize,
                expectedRawBytes);
        }
        pledgedSize = pledgedSizeResult.Value();

        const std::size_t boundResult =
            ZSTD_compressBound(pledgedSize.value());
        if (ZSTD_isError(boundResult))
        {
            const CompressionError mappedError =
                detail::MapZstdFunctionResult(boundResult);
            return CompressionResult<SegmentCompressor>::Failure(
                mappedError.code, mappedError.detail);
        }
        const std::optional<std::uint64_t> boundBytesResult =
            NarrowSizeToUint64(boundResult);
        if (!boundBytesResult.has_value())
        {
            return CompressionResult<SegmentCompressor>::Failure(
                CompressionErrorCode::InvalidExpectedRawSize,
                expectedRawBytes);
        }
        initialBytes = std::min(
            boundBytesResult.value(),
            settings.maxOutputBytes);
    }
    else
    {
        initialBytes = std::min(
            kInitialStreamBytes, settings.maxOutputBytes);
    }
    initialBytes = std::max(initialBytes, kMinFrameBytes);

    const auto initialSizeResult =
        pbprotocol::CheckedUint64ToSize(initialBytes);
    if (!initialSizeResult || !FitsByteVector(initialBytes))
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            settings.maxOutputBytes);
    }

    SegmentCompressor compressor;
    try
    {
        compressor.outputBuffer_.resize(initialSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            settings.maxOutputBytes);
    }

    using CompressContextPointer =
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>;
    CompressContextPointer compressContext(
        ZSTD_createCCtx(), &ZSTD_freeCCtx);
    if (compressContext == nullptr)
    {
        return CompressionResult<SegmentCompressor>::Failure(
            CompressionErrorCode::AllocationFailure);
    }

    auto mapFailure = [](const std::size_t functionResult)
    {
        const CompressionError mappedError =
            detail::MapZstdFunctionResult(functionResult);
        return CompressionResult<SegmentCompressor>::Failure(
            mappedError.code, mappedError.detail);
    };

    const std::size_t levelResult = ZSTD_CCtx_setParameter(
        compressContext.get(),
        ZSTD_c_compressionLevel,
        settings.compressionLevel);
    if (ZSTD_isError(levelResult))
    {
        return mapFailure(levelResult);
    }

    const std::size_t windowResult = ZSTD_CCtx_setParameter(
        compressContext.get(),
        ZSTD_c_windowLog,
        static_cast<int>(settings.maxWindowLog));
    if (ZSTD_isError(windowResult))
    {
        return mapFailure(windowResult);
    }

    // A 32-bit frame checksum makes single-bit corruption fail
    // deterministically instead of decoding into plausible garbage.
    const std::size_t checksumResult = ZSTD_CCtx_setParameter(
        compressContext.get(), ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(checksumResult))
    {
        return mapFailure(checksumResult);
    }

    if (expectedRawBytes != 0)
    {
        const std::size_t pledgedResult = ZSTD_CCtx_setPledgedSrcSize(
            compressContext.get(), expectedRawBytes);
        if (ZSTD_isError(pledgedResult))
        {
            return mapFailure(pledgedResult);
        }
    }

    compressor.settings_ = settings;
    compressor.expectedRawBytes_ = expectedRawBytes;
    compressor.compressContext_ =
        static_cast<void*>(compressContext.release());
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
    const std::optional<std::uint64_t> currentBytesResult =
        NarrowSizeToUint64(outputBuffer_.size());
    const std::optional<std::uint64_t> outputBytesResult =
        NarrowSizeToUint64(outputSize_);
    if (!currentBytesResult.has_value()
        || !outputBytesResult.has_value())
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidState);
    }
    const std::uint64_t currentBytes = currentBytesResult.value();
    const std::uint64_t outputBytes = outputBytesResult.value();
    const std::uint64_t maximumBytes = settings_.maxOutputBytes;
    if (currentBytes > maximumBytes || outputBytes > currentBytes)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidState);
    }

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
        outputBytes, minimumFreeBytes);
    if (!minimumBytesResult)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::CompressedFrameLimitExceeded,
            outputBytes);
    }
    if (minimumBytesResult.Value() > desiredBytes)
    {
        desiredBytes = minimumBytesResult.Value();
    }

    if (desiredBytes > maximumBytes || desiredBytes <= currentBytes)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::CompressedFrameLimitExceeded,
            maximumBytes);
    }

    const auto desiredSizeResult =
        pbprotocol::CheckedUint64ToSize(desiredBytes);
    if (!desiredSizeResult || !FitsByteVector(desiredBytes))
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            maximumBytes);
    }
    try
    {
        outputBuffer_.resize(desiredSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::AllocationFailure, 0);
    }
    catch (const std::length_error&)
    {
        return CompressionStatus::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            maximumBytes);
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

        const std::optional<std::uint64_t> consumedBytesResult =
            NarrowSizeToUint64(inputBuffer.pos - consumedBefore);
        if (!consumedBytesResult.has_value())
        {
            const CompressionError fedError{
                CompressionErrorCode::InvalidExpectedRawSize,
                fedBytes_};
            terminalError_ = fedError;
            return CompressionStatus::Failure(
                fedError.code, fedError.detail);
        }
        const std::uint64_t consumedBytes = consumedBytesResult.value();
        if (consumedBytes == 0 && outputChunk.pos == 0)
        {
            // Defensive: with both remaining input and free output space
            // zstd must consume input, emit output, or report an error.
            // This is an internal state violation, not evidence that the
            // configured frame cap was reached, so it must not trigger RAW
            // fallback.
            const CompressionError stalledError{
                CompressionErrorCode::ZstdError, 0};
            terminalError_ = stalledError;
            return CompressionStatus::Failure(
                stalledError.code, stalledError.detail);
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

        const auto outputSizeResult = pbprotocol::CheckedAddSize(
            outputSize_, outputChunk.pos);
        if (!outputSizeResult
            || outputSizeResult.Value() > outputBuffer_.size())
        {
            const CompressionError outputError{
                CompressionErrorCode::ZstdError, outputSize_};
            terminalError_ = outputError;
            return CompressionStatus::Failure(
                outputError.code, outputError.detail);
        }
        outputSize_ = outputSizeResult.Value();
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

        const auto outputSizeResult = pbprotocol::CheckedAddSize(
            outputSize_, outputChunk.pos);
        if (!outputSizeResult
            || outputSizeResult.Value() > outputBuffer_.size())
        {
            const CompressionError outputError{
                CompressionErrorCode::ZstdError, outputSize_};
            terminalError_ = outputError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                outputError.code, outputError.detail);
        }
        outputSize_ = outputSizeResult.Value();

        if (flushResult == 0)
        {
            // ZSTD_e_end returns 0 exactly when the frame is complete and
            // the output buffer has been fully flushed.
            try
            {
                outputBuffer_.resize(outputSize_);
            }
            catch (const std::bad_alloc&)
            {
                const CompressionError allocationError{
                    CompressionErrorCode::AllocationFailure, 0};
                terminalError_ = allocationError;
                return CompressionResult<std::vector<std::byte>>::Failure(
                    allocationError.code, allocationError.detail);
            }
            catch (const std::length_error&)
            {
                const CompressionError sizeError{
                    CompressionErrorCode::InvalidMaxOutputBytes,
                    settings_.maxOutputBytes};
                terminalError_ = sizeError;
                return CompressionResult<std::vector<std::byte>>::Failure(
                    sizeError.code, sizeError.detail);
            }
            frameFinished_ = true;
            return CompressionResult<std::vector<std::byte>>::Success(
                std::move(outputBuffer_));
        }

        if (outputChunk.pos == 0)
        {
            // Free space was just grown (or already existed) yet nothing
            // was produced: a stalled end flush cannot be fixed by growing
            // again. This is not a proven frame-cap exhaustion and therefore
            // must not silently select RAW.
            const CompressionError stalledError{
                CompressionErrorCode::ZstdError, 0};
            terminalError_ = stalledError;
            return CompressionResult<std::vector<std::byte>>::Failure(
                stalledError.code, stalledError.detail);
        }
    }
}

CompressionResult<EncodedSegment> CompressSegment(
    const std::span<const std::byte> input,
    const CompressionSettings& settings)
{
    const CompressionStatus settingsStatus =
        ValidateCompressionSettings(settings);
    if (!settingsStatus)
    {
        return CompressionResult<EncodedSegment>::Failure(
            settingsStatus.Error().code,
            settingsStatus.Error().detail);
    }

    if (input.empty())
    {
        // Empty segments stay Raw with zero bytes, which also satisfies
        // the descriptor rule encodedSize == rawSize for Raw segments.
        return CopyRawSegment(input, settings);
    }

    const std::optional<std::uint64_t> rawBytesResult =
        NarrowSizeToUint64(input.size());
    if (!rawBytesResult.has_value())
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::InvalidExpectedRawSize);
    }
    const std::uint64_t rawBytes = rawBytesResult.value();

    auto compressorResult = SegmentCompressor::Create(
        settings, rawBytes);
    if (!compressorResult)
    {
        return CompressionResult<EncodedSegment>::Failure(
            compressorResult.Error().code, compressorResult.Error().detail);
    }
    SegmentCompressor compressor = std::move(compressorResult).Value();

    const CompressionStatus updateStatus = compressor.Update(input);
    if (!updateStatus)
    {
        if (updateStatus.Error().code
            == CompressionErrorCode::CompressedFrameLimitExceeded)
        {
            return CopyRawSegment(input, settings);
        }
        return CompressionResult<EncodedSegment>::Failure(
            updateStatus.Error().code, updateStatus.Error().detail);
    }

    auto frameResult = compressor.Finish();
    if (!frameResult)
    {
        if (frameResult.Error().code
            == CompressionErrorCode::CompressedFrameLimitExceeded)
        {
            return CopyRawSegment(input, settings);
        }
        return CompressionResult<EncodedSegment>::Failure(
            frameResult.Error().code, frameResult.Error().detail);
    }

    const std::optional<std::uint64_t> frameBytesResult =
        NarrowSizeToUint64(frameResult.Value().size());
    if (!frameBytesResult.has_value())
    {
        return CompressionResult<EncodedSegment>::Failure(
            CompressionErrorCode::InvalidMaxOutputBytes,
            settings.maxOutputBytes);
    }
    const pbprotocol::CompressionCodec codec = ChooseSegmentCodec(
        frameBytesResult.value(),
        rawBytes,
        settings.framingMarginBytes);

    EncodedSegment encodedSegment;
    encodedSegment.codec = codec;
    if (codec == pbprotocol::CompressionCodec::Zstandard)
    {
        encodedSegment.bytes = std::move(frameResult).Value();
    }
    else
    {
        return CopyRawSegment(input, settings);
    }
    return CompressionResult<EncodedSegment>::Success(
        std::move(encodedSegment));
}

} // namespace pbcompression
