#pragma once
// Hand-crafted minimal PNG builder for the PBModulation PNG decode tests.
//
// Independent of libpng: it implements the PNG specification structures
// directly (signature, chunks, CRC-32, zlib stored blocks, scanline
// filters 0-4, Adam7 interlace, palette/tRNS, 1/2/4/8/16-bit depths) so
// that the library decoder is validated against the specification rather
// than against the library's own encoder.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace pngoracle {

// zlib CRC-32 (reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR
// 0xFFFFFFFF). Distinct from the protocol CRC-32C used elsewhere.
[[nodiscard]] inline std::uint32_t Crc32(
    const std::span<const std::byte> data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte value : data)
    {
        std::uint8_t octet = std::to_integer<std::uint8_t>(value);
        crc ^= octet;
        for (int bit = 0; bit < 8; bit++)
        {
            const std::uint32_t mask =
                (crc & 1u) != 0u ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Adler-32 checksum (zlib trailer).
[[nodiscard]] inline std::uint32_t Adler32(
    const std::span<const std::byte> data)
{
    std::uint32_t a = 1u;
    std::uint32_t b = 0u;
    for (const std::byte value : data)
    {
        a = (a + std::to_integer<std::uint8_t>(value)) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// zlib stream consisting only of stored (uncompressed) blocks.
[[nodiscard]] inline std::vector<std::byte> BuildStoredZlib(
    const std::span<const std::byte> data)
{
    std::vector<std::byte> out;
    out.push_back(std::byte{0x08});
    out.push_back(std::byte{0x1D});
    const std::size_t totalBytes = data.size();
    std::size_t offset = 0;
    bool lastBlock = false;
    while (!lastBlock)
    {
        const std::size_t blockBytes =
            (totalBytes - offset) > 65535u ? 65535u
                                           : (totalBytes - offset);
        lastBlock = offset + blockBytes == totalBytes;
        const std::uint16_t length = static_cast<std::uint16_t>(blockBytes);
        const std::uint16_t complement = static_cast<std::uint16_t>(
            ~length);
        // Stored deflate block: one header byte (BFINAL = bit 0, BTYPE =
        // 00 in bits 1-2 per RFC 1951), then the 16-bit length and its
        // one's complement, little-endian.
        out.push_back(std::byte{static_cast<std::uint8_t>(
            lastBlock ? 0x01u : 0x00u)});
        out.push_back(std::byte{static_cast<std::uint8_t>(length & 0xFFu)});
        out.push_back(std::byte{
            static_cast<std::uint8_t>((length >> 8) & 0xFFu)});
        out.push_back(std::byte{static_cast<std::uint8_t>(complement & 0xFFu)});
        out.push_back(std::byte{
            static_cast<std::uint8_t>((complement >> 8) & 0xFFu)});
        out.insert(out.end(), data.begin() + offset,
            data.begin() + offset + blockBytes);
        offset += blockBytes;
    }
    const std::uint32_t adler = Adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        out.push_back(std::byte{
            static_cast<std::uint8_t>((adler >> shift) & 0xFFu)});
    }
    return out;
}

struct PngFormat
{
    int bitDepth = 8; // 1, 2, 4, 8 or 16
    int colorType = 6; // 0 gray, 2 rgb, 3 palette, 4 gray+alpha, 6 rgba
    bool interlaced = false; // Adam7
    // colorType 3: 3 bytes per entry (at most 2^bitDepth entries).
    std::vector<std::uint8_t> palette;
    // Optional tRNS: palette -> one byte per entry; gray -> 2 bytes;
    // RGB -> 6 bytes.
    std::vector<std::uint8_t> tRns;
};

// Pixel content provider: fills pixelBytes with the raw PNG-storage bytes
// of pixel (x, y): 8-bit RGBA=4 (R,G,B,A), RGB=3, gray=1, gray+alpha=2,
// palette index=1; 16-bit values are big-endian per channel. For sub-byte
// depths (1/2/4-bit, single channel) only pixelValue[0] must carry the raw
// sample value (0..2^bitDepth-1); the builder packs the bits MSB-first.
using PixelBytesProvider = std::function<void(
    std::uint32_t x,
    std::uint32_t y,
    std::uint8_t* pixelBytes)>;

namespace detail {

[[nodiscard]] inline int NumChannels(const int colorType)
{
    switch (colorType)
    {
        case 0:
        case 3:
            return 1;
        case 4:
            return 2;
        case 2:
            return 3;
        case 6:
            return 4;
        default:
            return 0;
    }
}

// Integer bytes from sample to sample (spec: bit count rounded up to a
// multiple of 8, divided by 8; minimum 1).
[[nodiscard]] inline int FilterBpp(const PngFormat& format)
{
    const int bitsPerPixel = format.bitDepth * NumChannels(format.colorType);
    return (bitsPerPixel + 7) / 8;
}

[[nodiscard]] inline int PaethPredictor(
    const int a,
    const int b,
    const int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
    {
        return a;
    }
    if (pb <= pc)
    {
        return b;
    }
    return c;
}

// Applies the forward scanline filter to one raw row (mod 256).
inline void ApplyFilter(
    const std::vector<std::uint8_t>& rawRow,
    const std::span<const std::uint8_t> previousRow,
    const int filterMode,
    const int bpp,
    std::vector<std::uint8_t>& filteredRow)
{
    filteredRow.assign(rawRow.size(), 0);
    for (std::size_t i = 0; i < rawRow.size(); i++)
    {
        const int a = i >= static_cast<std::size_t>(bpp)
            ? rawRow[i - static_cast<std::size_t>(bpp)]
            : 0;
        const int b = !previousRow.empty()
            ? previousRow[i % previousRow.size()]
            : 0;
        const int c = !previousRow.empty() &&
            i >= static_cast<std::size_t>(bpp)
            ? previousRow[(i - static_cast<std::size_t>(bpp)) %
                previousRow.size()]
            : 0;
        int prediction = 0;
        switch (filterMode)
        {
            case 1:
                prediction = a;
                break;
            case 2:
                prediction = b;
                break;
            case 3:
                prediction = (a + b) / 2;
                break;
            case 4:
                prediction = PaethPredictor(a, b, c);
                break;
            default:
                prediction = 0;
                break;
        }
        filteredRow[i] =
            static_cast<std::uint8_t>((rawRow[i] - prediction) & 0xFF);
    }
}

} // namespace detail

// Builds a complete PNG file. filterMode (0 None, 1 Sub, 2 Up, 3 Avg,
// 4 Paeth) is applied to every row of every interlace pass.
[[nodiscard]] inline std::vector<std::byte> BuildPng(
    const std::uint32_t width,
    const std::uint32_t height,
    const PngFormat& format,
    const int filterMode,
    const PixelBytesProvider& pixelBytes)
{
    const int channels = detail::NumChannels(format.colorType);
    if (channels == 0)
    {
        throw std::runtime_error("invalid PNG color type");
    }
    const int bitsPerPixel = format.bitDepth * channels;
    const int bytesPerPixel =
        (format.bitDepth * channels) % 8 == 0
            ? (format.bitDepth * channels) / 8
            : 1;

    auto MakePassImage = [&](const std::uint32_t x0,
        const std::uint32_t y0,
        const std::uint32_t xStep,
        const std::uint32_t yStep,
        std::vector<std::uint8_t>& outRaw,
        std::uint32_t& outWidth,
        std::uint32_t& outHeight)
    {
        outWidth = width > x0 ? (width - x0 + xStep - 1) / xStep : 0;
        outHeight = height > y0 ? (height - y0 + yStep - 1) / yStep : 0;
        if (outWidth == 0 || outHeight == 0)
        {
            outRaw.clear();
            return;
        }
        const std::size_t rowBytes =
            (bitsPerPixel * static_cast<int>(outWidth) + 7) / 8;
        outRaw.assign(static_cast<std::size_t>(outHeight) * rowBytes, 0);
        const bool subByteSample = (format.bitDepth % 8) != 0;
        std::uint8_t pixelValue[16] = {0};
        for (std::uint32_t passY = 0; passY < outHeight; passY++)
        {
            for (std::uint32_t passX = 0; passX < outWidth; passX++)
            {
                const std::uint32_t sourceX = x0 + passX * xStep;
                const std::uint32_t sourceY = y0 + passY * yStep;
                pixelBytes(sourceX, sourceY, pixelValue);
                const std::size_t rowBase =
                    static_cast<std::size_t>(passY) * rowBytes;
                if (!subByteSample)
                {
                    const std::size_t base = rowBase +
                        static_cast<std::size_t>(passX) *
                            static_cast<std::size_t>(bytesPerPixel);
                    std::copy(pixelValue, pixelValue + bytesPerPixel,
                        outRaw.begin() + base);
                }
                else
                {
                    // Sub-byte samples are packed MSB-first into the row
                    // bytes per the PNG specification; one sample occupies
                    // bitDepth bits starting at bitOffset.
                    const int bitOffset =
                        static_cast<int>(passX) * bitsPerPixel;
                    const std::size_t byteIndex = rowBase +
                        static_cast<std::size_t>(bitOffset / 8);
                    const int shift =
                        8 - (bitOffset % 8) - bitsPerPixel;
                    const std::uint8_t sampleMask =
                        static_cast<std::uint8_t>(
                            (1u << bitsPerPixel) - 1u);
                    outRaw[byteIndex] = static_cast<std::uint8_t>(
                        outRaw[byteIndex] |
                        ((pixelValue[0] & sampleMask) << shift));
                }
            }
        }
    };

    // Adam7 pass grid (x0, y0, xStep, yStep), identical to the tables
    // libpng uses for PNG_INTERLACE_ADAM7 (png_pass_start / ystart /
    // inc / yinc). Passes 4-7 of the earlier draft table were wrong
    // (they never cover row 0 fully); this exact grid is what any
    // conformant decoder expects.
    static constexpr std::array<std::array<std::uint32_t, 4>, 7> kAdam7Passes =
        {{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
          {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};

    std::vector<std::uint8_t> idatPayload;
    const int numPasses = format.interlaced ? 7 : 1;
    for (int passIndex = 0; passIndex < numPasses; passIndex++)
    {
        const auto& pass = format.interlaced
            ? kAdam7Passes[static_cast<std::size_t>(passIndex)]
            : std::array<std::uint32_t, 4>{0, 0, 1, 1};
        std::vector<std::uint8_t> rawImage;
        std::uint32_t passWidth = 0;
        std::uint32_t passHeight = 0;
        MakePassImage(pass[0], pass[1], pass[2], pass[3], rawImage,
            passWidth, passHeight);
        if (passWidth == 0 || passHeight == 0)
        {
            continue;
        }
        const std::size_t rowBytes =
            (bitsPerPixel * static_cast<int>(passWidth) + 7) / 8;
        const int bpp = detail::FilterBpp(format);
        // The previous row's bytes must outlive the loop iteration: the
        // filter prediction reads them on the next pass, so they are kept in
        // a dedicated buffer (a span into the per-iteration rawRow would
        // dangle after that iteration).
        std::vector<std::uint8_t> previousRawRow;
        std::span<const std::uint8_t> previousRow;
        for (std::uint32_t row = 0; row < passHeight; row++)
        {
            const auto rowBegin =
                rawImage.begin() + static_cast<std::size_t>(row) * rowBytes;
            const std::vector<std::uint8_t> rawRow(
                rowBegin, rowBegin + static_cast<std::ptrdiff_t>(rowBytes));
            std::vector<std::uint8_t> filteredRow;
            detail::ApplyFilter(rawRow, previousRow, filterMode, bpp,
                filteredRow);
            idatPayload.push_back(
                static_cast<std::uint8_t>(filterMode));
            idatPayload.insert(idatPayload.end(), filteredRow.begin(),
                filteredRow.end());
            previousRawRow = rawRow;
            previousRow = std::span<const std::uint8_t>(previousRawRow);
        }
    }

    const std::vector<std::byte> idatBytes = BuildStoredZlib(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(idatPayload.data()),
            idatPayload.size()));

    std::vector<std::byte> png;
    auto appendBytes = [&png](const std::span<const std::byte> bytes)
    {
        png.insert(png.end(), bytes.begin(), bytes.end());
    };
    auto appendUint32BigEndian = [&png](const std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            png.push_back(std::byte{
                static_cast<std::uint8_t>((value >> shift) & 0xFFu)});
        }
    };
    auto appendChunk =
        [&png, &appendBytes, &appendUint32BigEndian](
            const char type[4],
            const std::span<const std::byte> data)
    {
        appendUint32BigEndian(
            static_cast<std::uint32_t>(data.size()));
        const std::span<const std::byte> typeBytes(
            reinterpret_cast<const std::byte*>(type), 4);
        std::vector<std::byte> crcInput;
        crcInput.insert(crcInput.end(), typeBytes.begin(), typeBytes.end());
        crcInput.insert(crcInput.end(), data.begin(), data.end());
        appendBytes(typeBytes);
        appendBytes(data);
        const std::uint32_t crc = Crc32(crcInput);
        appendUint32BigEndian(crc);
    };

    // Signature.
    appendBytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>("\x89PNG\r\n\x1a\n"), 8));

    // IHDR.
    std::vector<std::byte> ihdr;
    ihdr.reserve(13);
    auto pushUint32 = [&ihdr](const std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            ihdr.push_back(std::byte{
                static_cast<std::uint8_t>((value >> shift) & 0xFFu)});
        }
    };
    pushUint32(width);
    pushUint32(height);
    ihdr.push_back(std::byte{static_cast<std::uint8_t>(format.bitDepth)});
    ihdr.push_back(std::byte{static_cast<std::uint8_t>(format.colorType)});
    ihdr.push_back(std::byte{0}); // compression
    ihdr.push_back(std::byte{0}); // filter
    ihdr.push_back(std::byte{
        static_cast<std::uint8_t>(format.interlaced ? 1 : 0)});
    appendChunk("IHDR", ihdr);

    if (!format.palette.empty())
    {
        const std::byte* paletteData =
            reinterpret_cast<const std::byte*>(format.palette.data());
        const std::vector<std::byte> paletteBytes(
            paletteData, paletteData + format.palette.size());
        appendChunk("PLTE", paletteBytes);
    }
    if (!format.tRns.empty())
    {
        const std::byte* tRnsData =
            reinterpret_cast<const std::byte*>(format.tRns.data());
        const std::vector<std::byte> tRnsBytes(
            tRnsData, tRnsData + format.tRns.size());
        appendChunk("tRNS", tRnsBytes);
    }
    appendChunk("IDAT", idatBytes);
    appendChunk("IEND", {});
    return png;
}

} // namespace pngoracle