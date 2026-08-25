#include "pbmodulation/frame_io.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace pbmodulation {
namespace {

using pbprotocol::CheckedMultiplyUnsigned;

// Frozen encoder parameters (design 38.2): the Golden Vector PNG digest is
// reproducible only with these exact settings plus the pinned libpng 1.6.58
// and zlib 1.3.2 (see docs/REFERENCE_RASTER.md).
constexpr int kPngCompressionLevel = 6;
constexpr std::uint8_t kPngAlphaFiller = 0xFF;

// libpng 1.6: when no error handler is installed, png_error() falls through
// to PNG_ABORT() (process abort) instead of longjmp. Both contexts therefore
// install the longjmp handler so that malformed input always fails closed
// with a ModulationError instead of terminating the process.
[[noreturn]] void PngErrorHandler(
    png_structp png,
    png_const_charp message)
{
    (void)message;
    longjmp(png_jmpbuf(png), 1);
}

void PngWarningHandler(
    png_structp png,
    png_const_charp message)
{
    (void)png;
    (void)message;
}

struct PngReadContext
{
    std::span<const std::byte> bytes;
    std::size_t position = 0;
};

// Plain-data write target: the libpng callback must never unwind through the
// C call frames (a std::vector insert can throw bad_alloc), so the encoder
// pre-allocates the output and the callback only advances a byte pointer.
// Exceeding the frozen deflate upper bound is a protocol error, not an
// allocation opportunity: fail closed via png_error (longjmp).
struct PngWriteContext
{
    std::byte* buffer = nullptr;
    std::size_t capacity = 0;
    std::size_t position = 0;
};

void PngWriteCallback(
    png_structp png,
    png_bytep data,
    png_size_t size)
{
    auto* context = static_cast<PngWriteContext*>(png_get_io_ptr(png));
    if (context->position + size > context->capacity)
    {
        png_error(png, "PNG output upper bound exceeded");
    }
    std::memcpy(context->buffer + context->position, data, size);
    context->position += size;
}

void PngReadCallback(
    png_structp png,
    png_bytep data,
    png_size_t size)
{
    auto* context = static_cast<PngReadContext*>(png_get_io_ptr(png));
    if (context->position + size > context->bytes.size())
    {
        png_error(png, "PNG stream truncated");
    }
    std::memcpy(data, context->bytes.data() + context->position, size);
    context->position += size;
}

// RAII cleanup for libpng contexts so that every exit path (success,
// setjmp error, or C++ exception) releases the structures exactly once.
// Non-trivial locals are declared before the setjmp below so a longjmp
// back to the setjmp point still leaves them on the live stack; their
// destructors then run once at function exit.
struct PngDestroyContext
{
    bool isRead = false;
    png_structp pngPtr = nullptr;
    png_infop infoPtr = nullptr;

    ~PngDestroyContext()
    {
        if (pngPtr == nullptr)
        {
            return;
        }
        if (isRead)
        {
            png_destroy_read_struct(&pngPtr, &infoPtr, nullptr);
        }
        else
        {
            png_destroy_write_struct(&pngPtr, &infoPtr);
        }
        pngPtr = nullptr;
    }
};

// Validates frame dimensions against the decode-side bounds and computes
// the exact pixel byte count with checked arithmetic.
[[nodiscard]] ModulationStatus ValidateFrameDimensions(
    const std::uint32_t width,
    const std::uint32_t height,
    std::uint64_t& outPixelBytes) noexcept
{
    if (width < 1 || height < 1 ||
        width > kMaximumFrameDimension || height > kMaximumFrameDimension)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }
    const auto areaResult =
        CheckedMultiplyUnsigned<std::uint64_t>(width, height);
    if (!areaResult)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto pixelBytesResult =
        CheckedMultiplyUnsigned<std::uint64_t>(areaResult.Value(), 4);
    if (!pixelBytesResult)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    outPixelBytes = pixelBytesResult.Value();
    return ModulationStatus::Success();
}

// Worst-case PNG output size for the frozen filter-none encode: per-row
// filter byte plus RGBA row. The zlib stream then expands the raw stream by
// the per-deflate-block overhead: zlib 1.3.2 may choose Huffman blocks whose
// code-tree bytes expand incompressible input by a few hundred bytes per
// 65535-byte block (the measured worst case for the frozen 1920x1080 canvas
// with incompressible content is a 0.0018 expansion, see
// docs/REFERENCE_RASTER.md). raw/32 keeps a wide finite margin above that,
// and the fixed term covers signature plus chunk framing.
[[nodiscard]] bool ComputePngOutputUpperBound(
    const std::uint32_t width,
    const std::uint32_t height,
    std::uint64_t& outUpperBoundBytes) noexcept
{
    const auto rowBytesResult =
        CheckedMultiplyUnsigned<std::uint64_t>(width, 4);
    if (!rowBytesResult)
    {
        return false;
    }
    const std::uint64_t rawRowBytes = rowBytesResult.Value() + 1;
    const auto totalRawResult =
        CheckedMultiplyUnsigned<std::uint64_t>(rawRowBytes, height);
    if (!totalRawResult)
    {
        return false;
    }
    const std::uint64_t totalRaw = totalRawResult.Value();
    const std::uint64_t storedBlockHeaders =
        5u * (totalRaw / 65535u + 1u);
    const std::uint64_t expansionMargin = totalRaw / 32u;
    if (totalRaw >
        std::numeric_limits<std::uint64_t>::max() -
        storedBlockHeaders - expansionMargin - 4096u)
    {
        return false;
    }
    outUpperBoundBytes = totalRaw + storedBlockHeaders + expansionMargin +
        4096u;
    return true;
}

} // namespace

ModulationResult<std::vector<std::byte>> EncodeRawFrame(
    const std::span<const std::byte> bgra,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    std::uint64_t pixelBytes = 0;
    const auto dimensionsResult = ValidateFrameDimensions(width, height,
        pixelBytes);
    if (!dimensionsResult)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            dimensionsResult.Error().code,
            dimensionsResult.Error().offset);
    }
    if (bgra.size() != pixelBytes)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    std::vector<std::byte> raw;
    try
    {
        raw.resize(kRawFrameHeaderBytes + pixelBytes);
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::MemoryAllocationFailure,
            0);
    }
    std::span<std::byte> headerSpan{raw};
    pbprotocol::ByteWriter writer(
        headerSpan.first(kRawFrameHeaderBytes));
    std::array<std::byte, 3> reservedFront{};
    pbprotocol::ProtocolStatus status =
        writer.WriteFixedBytes(kRawFrameMagic);
    status = writer.WriteUint8(kRawFrameVersion);
    status = writer.WriteFixedBytes(reservedFront);
    status = writer.WriteUint32(width);
    status = writer.WriteUint32(height);
    status = writer.WriteUint32(static_cast<std::uint32_t>(pixelBytes));
    status = writer.WriteUint64(0);
    if (!status || writer.Position() != kRawFrameHeaderBytes)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::InternalInvariantViolation,
            writer.Position());
    }
    std::copy(bgra.begin(), bgra.end(), raw.begin() + kRawFrameHeaderBytes);
    return ModulationResult<std::vector<std::byte>>::Success(
        std::move(raw));
}

ModulationStatus DecodeRawFrame(
    const std::span<const std::byte> raw,
    const std::span<std::byte> outBgra,
    std::uint32_t& outWidth,
    std::uint32_t& outHeight) noexcept
{
    if (raw.size() < kRawFrameHeaderBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TruncatedInput,
            raw.size());
    }

    std::span<const std::byte> headerSpan{raw};
    pbprotocol::ByteReader reader(
        headerSpan.first(kRawFrameHeaderBytes));
    const auto magicResult = reader.ReadFixedBytes<4>();
    if (!magicResult || magicResult.Value() != kRawFrameMagic)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidMagic, 0);
    }
    const auto versionResult = reader.ReadUint8();
    if (!versionResult || versionResult.Value() != kRawFrameVersion)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::UnsupportedVersion,
            4);
    }
    const auto reservedFrontResult = reader.ReadFixedBytes<3>();
    if (!reservedFrontResult ||
        !std::all_of(
            reservedFrontResult.Value().begin(),
            reservedFrontResult.Value().end(),
            [](const std::byte value) { return value == std::byte{0}; }))
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::NonZeroReservedByte,
            5);
    }
    const auto widthResult = reader.ReadUint32();
    const auto heightResult = reader.ReadUint32();
    const auto pixelBytesResult = reader.ReadUint32();
    const auto reservedTailResult = reader.ReadUint64();
    if (!widthResult || !heightResult || !pixelBytesResult ||
        !reservedTailResult)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TruncatedInput,
            reader.Position());
    }
    if (reservedTailResult.Value() != 0)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::NonZeroReservedByte,
            20);
    }

    std::uint64_t expectedPixelBytes = 0;
    const auto dimensionsResult = ValidateFrameDimensions(
        widthResult.Value(),
        heightResult.Value(),
        expectedPixelBytes);
    if (!dimensionsResult)
    {
        return dimensionsResult;
    }
    if (static_cast<std::uint64_t>(pixelBytesResult.Value()) !=
        expectedPixelBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InvalidInput,
            16);
    }
    const std::size_t expectedTotalBytes =
        static_cast<std::size_t>(kRawFrameHeaderBytes + expectedPixelBytes);
    if (raw.size() < expectedTotalBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TruncatedInput,
            raw.size());
    }
    if (raw.size() > expectedTotalBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TrailingBytes,
            expectedTotalBytes);
    }
    if (outBgra.size() != expectedPixelBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::OutputBufferTooSmall,
            0);
    }

    std::copy(
        raw.begin() + kRawFrameHeaderBytes,
        raw.begin() + expectedTotalBytes,
        outBgra.begin());
    outWidth = widthResult.Value();
    outHeight = heightResult.Value();
    return ModulationStatus::Success();
}

ModulationResult<std::vector<std::byte>> EncodePngFrame(
    const std::span<const std::byte> bgra,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    std::uint64_t pixelBytes = 0;
    const auto dimensionsResult = ValidateFrameDimensions(width, height,
        pixelBytes);
    if (!dimensionsResult)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            dimensionsResult.Error().code,
            dimensionsResult.Error().offset);
    }
    if (bgra.size() != pixelBytes)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    std::uint64_t outputUpperBound = 0;
    if (!ComputePngOutputUpperBound(width, height, outputUpperBound))
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    std::vector<png_byte> rgba;
    std::vector<png_bytep> rows;
    std::vector<std::byte> out;
    try
    {
        rgba.resize(pixelBytes);
        rows.resize(height);
        out.resize(static_cast<std::size_t>(outputUpperBound));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::MemoryAllocationFailure,
            0);
    }

    // Plain-data context (declared before the setjmp below like every other
    // local); the callback mutates position through this pointer.
    PngWriteContext writeContext{out.data(), out.size(), 0};

    // BGRA -> RGBA at the format boundary (the canvas format is BGRA; PNG
    // truecolor stores R first).
    const auto* bgraBegin = bgra.data();
    for (std::size_t pixelIndex = 0;
        pixelIndex < pixelBytes / 4; pixelIndex++)
    {
        const std::size_t source = pixelIndex * 4;
        rgba[source] = std::to_integer<std::uint8_t>(bgraBegin[source + 2]);
        rgba[source + 1] = std::to_integer<std::uint8_t>(bgraBegin[source + 1]);
        rgba[source + 2] = std::to_integer<std::uint8_t>(bgraBegin[source]);
        rgba[source + 3] = std::to_integer<std::uint8_t>(bgraBegin[source + 3]);
    }
    for (std::uint32_t rowIndex = 0; rowIndex < height; rowIndex++)
    {
        rows[rowIndex] = rgba.data() +
            static_cast<std::size_t>(rowIndex) * width * 4;
    }

    PngDestroyContext context;
    context.pngPtr = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, nullptr, PngErrorHandler,
        PngWarningHandler);
    if (context.pngPtr == nullptr)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::PngEncodeError,
            0);
    }
    context.infoPtr = png_create_info_struct(context.pngPtr);
    if (context.infoPtr == nullptr)
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::PngEncodeError,
            0);
    }
#if defined(_MSC_VER)
    // MSVC C4611 flags setjmp used with C++ objects. The use is safe here:
    // every non-trivial local (rgba, rows, out, context) is declared before
    // the setjmp, so a longjmp back to it leaves them on the live stack and
    // their destructors run exactly once at function exit.
    #   pragma warning(push)
    #   pragma warning(disable : 4611)
    if (setjmp(png_jmpbuf(context.pngPtr)))
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::PngEncodeError,
            0);
    }
    #   pragma warning(pop)
#else
    if (setjmp(png_jmpbuf(context.pngPtr)))
    {
        return ModulationResult<std::vector<std::byte>>::Failure(
            ModulationErrorCode::PngEncodeError,
            0);
    }
#endif
    png_set_IHDR(
        context.pngPtr,
        context.infoPtr,
        width,
        height,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(context.pngPtr, kPngCompressionLevel);
    png_set_filter(context.pngPtr, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    png_set_write_fn(context.pngPtr, &writeContext, PngWriteCallback,
        nullptr);
    // The write callback only stores into the pre-allocated `out` buffer, so
    // no C++ exception can cross the libpng C call frames; encoder failures
    // surface through the longjmp handler installed above.
    png_write_info(context.pngPtr, context.infoPtr);
    png_write_image(context.pngPtr, rows.data());
    png_write_end(context.pngPtr, context.infoPtr);
    out.resize(writeContext.position);
    return ModulationResult<std::vector<std::byte>>::Success(
        std::move(out));
}

ModulationStatus DecodePngFrame(
    const std::span<const std::byte> png,
    const std::uint32_t expectedWidth,
    const std::uint32_t expectedHeight,
    const std::span<std::byte> outBgra) noexcept
{
    std::uint64_t expectedPixelBytes = 0;
    const auto dimensionsResult = ValidateFrameDimensions(
        expectedWidth,
        expectedHeight,
        expectedPixelBytes);
    if (!dimensionsResult)
    {
        return dimensionsResult;
    }
    if (outBgra.size() != expectedPixelBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::OutputBufferTooSmall,
            0);
    }
    if (png.size() < 8)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TruncatedInput,
            png.size());
    }

    PngReadContext readContext{png, 0};
    PngDestroyContext context;
    std::vector<std::byte> rgbaFrame;
    context.isRead = true;
    context.pngPtr = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, &readContext, PngErrorHandler,
        PngWarningHandler);
    if (context.pngPtr == nullptr)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::PngDecodeError,
            0);
    }
    context.infoPtr = png_create_info_struct(context.pngPtr);
    if (context.infoPtr == nullptr)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::PngDecodeError,
            0);
    }
#if defined(_MSC_VER)
    // See the encode side for why this setjmp is safe with C++ locals.
    #   pragma warning(push)
    #   pragma warning(disable : 4611)
    if (setjmp(png_jmpbuf(context.pngPtr)))
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::PngDecodeError,
            0);
    }
    #   pragma warning(pop)
#else
    if (setjmp(png_jmpbuf(context.pngPtr)))
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::PngDecodeError,
            0);
    }
#endif
    png_set_read_fn(context.pngPtr, &readContext, PngReadCallback);
    try
    {
        png_read_info(context.pngPtr, context.infoPtr);

        png_uint_32 width = 0;
        png_uint_32 height = 0;
        int bitDepth = 0;
        int colorType = 0;
        int interlaceType = 0;
        png_get_IHDR(context.pngPtr, context.infoPtr, &width, &height,
            &bitDepth, &colorType, &interlaceType, nullptr, nullptr);
        if (width != expectedWidth || height != expectedHeight)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::FrameGeometryMismatch,
                0);
        }

        // Normalize every supported 8/16-bit input to 8-bit RGBA. The
        // application order is fixed inside libpng (EXPAND first, then
        // 16_TO_8, then PACK, then FILLER), so the calls below only select
        // the transform set: 1/2/4-bit gray and palette samples expand to
        // 8-bit, tRNS becomes a real alpha channel instead of being
        // dropped, 16-bit samples strip to the high byte, and gray gains
        // RGB. png_set_add_alpha (FILLER + ADD_ALPHA) is required, not
        // png_set_filler: libpng only upgrades the transformed color type
        // to RGBA when ADD_ALPHA is also set (png_read_transform_info),
        // so a plain filler would leave an alpha-less RGB file reporting
        // color type RGB even though its rows carry the 0xFF filler byte.
        // The interlace transform must be selected before
        // png_read_update_info so the reported rowbytes cover the full
        // (de-interlaced) width; for non-interlaced input this is a
        // no-op that reports a single pass.
        const int numPasses = png_set_interlace_handling(context.pngPtr);
        png_set_expand_gray_1_2_4_to_8(context.pngPtr);
        png_set_packing(context.pngPtr);
        png_set_expand(context.pngPtr);
        png_set_tRNS_to_alpha(context.pngPtr);
        png_set_gray_to_rgb(context.pngPtr);
        png_set_add_alpha(context.pngPtr, kPngAlphaFiller, PNG_FILLER_AFTER);
        png_set_strip_16(context.pngPtr);
        png_read_update_info(context.pngPtr, context.infoPtr);

        const int updatedBitDepth =
            png_get_bit_depth(context.pngPtr, context.infoPtr);
        const int updatedColorType =
            png_get_color_type(context.pngPtr, context.infoPtr);
        if (updatedBitDepth != 8 ||
            updatedColorType != PNG_COLOR_TYPE_RGBA)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::UnsupportedPngFormat,
                0);
        }
        const png_size_t rowBytes =
            png_get_rowbytes(context.pngPtr, context.infoPtr);
        if (static_cast<std::uint64_t>(rowBytes) !=
            static_cast<std::uint64_t>(expectedWidth) * 4u)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::UnsupportedPngFormat,
                0);
        }

        // With the interlace transform active, png_read_row adds only
        // the pixels of the current Adam7 pass to the caller row buffer,
        // so it must be called once per (pass, row) and the buffer must
        // persist between passes. Rows are accumulated as 8-bit RGBA in
        // the private staging buffer and only copied (reordered) into
        // outBgra after every row read has succeeded, so a mid-stream
        // failure never leaves outBgra partially written.
        const std::uint64_t frameBytes =
            static_cast<std::uint64_t>(rowBytes) *
            static_cast<std::uint64_t>(height);
        rgbaFrame.resize(static_cast<std::size_t>(frameBytes));
        for (int passIndex = 0; passIndex < numPasses; passIndex++)
        {
            for (png_uint_32 rowIndex = 0; rowIndex < height; rowIndex++)
            {
                std::byte* rgbaRow = rgbaFrame.data() +
                    static_cast<std::size_t>(rowIndex) *
                    static_cast<std::size_t>(rowBytes);
                png_read_row(
                    context.pngPtr,
                    reinterpret_cast<png_bytep>(rgbaRow), nullptr);
            }
        }
        const std::size_t numPixels =
            static_cast<std::size_t>(
                static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height));
        for (std::size_t pixelIndex = 0; pixelIndex < numPixels; pixelIndex++)
        {
            const std::size_t source = pixelIndex * 4;
            // RGBA (as stored in PNG) -> BGRA (reference canvas format).
            outBgra[source] = rgbaFrame[source + 2];
            outBgra[source + 1] = rgbaFrame[source + 1];
            outBgra[source + 2] = rgbaFrame[source];
            outBgra[source + 3] = rgbaFrame[source + 3];
        }
        png_read_end(context.pngPtr, context.infoPtr);
    }
    catch (const std::bad_alloc&)
    {
        // png_read_info / png_read_row / rgbaFrame may allocate; a
        // failure rejects the frame instead of terminating the process.
        return ModulationStatus::Failure(
            ModulationErrorCode::MemoryAllocationFailure,
            0);
    }
    // Strict reference: no bytes after IEND are tolerated.
    if (readContext.position != png.size())
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::TrailingBytes,
            readContext.position);
    }
    return ModulationStatus::Success();
}

} // namespace pbmodulation