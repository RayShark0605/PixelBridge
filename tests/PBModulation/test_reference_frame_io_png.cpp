#include "modulation_test_helpers.h"
#include "png_oracle.h"

#include <png.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;
using pbmodulation::ModulationErrorCode;

namespace {

using ExpectedPixel = std::function<std::array<std::uint8_t, 4>(
    std::uint32_t, std::uint32_t)>;

// Deterministic RGBA pattern (R,G,B,A all in [0,255]).
std::array<std::uint8_t, 4> PatternRgba(
    const std::uint32_t x,
    const std::uint32_t y)
{
    return {
        static_cast<std::uint8_t>((x * 7u + y * 3u) & 0xFFu),
        static_cast<std::uint8_t>((x * 5u + y * 11u + 1u) & 0xFFu),
        static_cast<std::uint8_t>((x * 13u + y * 5u + 2u) & 0xFFu),
        static_cast<std::uint8_t>((x * 3u + y * 17u + 3u) & 0xFFu)};
}

// Deterministic grayscale pattern.
std::uint8_t PatternGray(const std::uint32_t x, const std::uint32_t y)
{
    return PatternRgba(x, y)[0];
}

pngoracle::PixelBytesProvider MakeRgbaProvider()
{
    return [](const std::uint32_t x, const std::uint32_t y,
        std::uint8_t* pixelBytes)
    {
        const std::array<std::uint8_t, 4> pixel = PatternRgba(x, y);
        for (std::size_t i = 0; i < 4; i++)
        {
            pixelBytes[i] = pixel[i];
        }
    };
}

pngoracle::PixelBytesProvider MakeGrayProvider()
{
    return [](const std::uint32_t x, const std::uint32_t y,
        std::uint8_t* pixelBytes)
    {
        pixelBytes[0] = PatternGray(x, y);
    };
}

// Decodes the oracle PNG and compares every pixel against the expected
// BGRA mapping (independent of the library encoder).
void VerifyOracleDecode(
    const char* const subCase,
    const std::vector<std::byte>& png,
    const std::uint32_t width,
    const std::uint32_t height,
    const ExpectedPixel& expectedPixel)
{
    std::vector<std::byte> bgra(
        static_cast<std::size_t>(width) * height * 4u);
    const auto status = DecodePngFrame(
        std::span<const std::byte>(png), width, height,
        std::span<std::byte>(bgra));
    INFO(subCase << " decodeCode=" << (status ? 0 : static_cast<int>(status.Error().code)) << " decodeOffset=" << (status ? 0 : status.Error().offset));
    REQUIRE(status);
    for (std::uint32_t y = 0; y < height; y++)
    {
        for (std::uint32_t x = 0; x < width; x++)
        {
            const std::array<std::uint8_t, 4> expected =
                expectedPixel(x, y);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            const std::uint8_t* actual =
                reinterpret_cast<const std::uint8_t*>(
                    bgra.data() + offset);
            const bool matches = actual[0] == expected[0] &&
                actual[1] == expected[1] && actual[2] == expected[2] &&
                actual[3] == expected[3];
            if (!matches)
            {
                FAIL(subCase << " oracle pixel mismatch at " << x << "," << y);
            }
        }
    }
}

// Small valid PNG used by the malformed matrix (8x5 RGBA, filter 0).
std::vector<std::byte> MakeSmallValidPng()
{
    return pngoracle::BuildPng(
        8, 5,
        pngoracle::PngFormat{},
        0,
        MakeRgbaProvider());
}

} // namespace

TEST_CASE("libpng is pinned to the frozen version",
    "[pbmodulation][png][pin]")
{
    // The manifest manifest can only express >= bounds; the exact pin is
    // enforced here so the PNG golden digest stays reproducible.
    CHECK(std::string_view(PNG_LIBPNG_VER_STRING) == "1.6.58");
}

TEST_CASE("PNG round-trips small and full-size frames",
    "[pbmodulation][png][roundtrip]")
{
    // 8x5 random RGBA.
    {
        std::vector<std::byte> bgra(8u * 5u * 4u);
        SplitMix64 random(0x5EED1234u);
        for (auto& value : bgra)
        {
            value = std::byte{static_cast<std::uint8_t>(
                random.Next() & 0xFFu)};
        }
        const auto encoded = EncodePngFrame(bgra, 8, 5);
        INFO("encodeCode=" << (encoded ? 0 : static_cast<int>(encoded.Error().code)) << " encodeOffset=" << (encoded ? 0 : encoded.Error().offset));
        REQUIRE(encoded);
        std::vector<std::byte> decoded(bgra.size());
        const auto status = DecodePngFrame(
            std::span<const std::byte>(encoded.Value()), 8, 5,
            std::span<std::byte>(decoded));
        REQUIRE(status);
        CHECK(decoded == bgra);
    }

    // 1920x1080 random RGBA (incompressible worst case for the
    // filter-none frozen encode).
    {
        std::vector<std::byte> bgra(1920u * 1080u * 4u);
        SplitMix64 random(0x5EED9999u);
        for (auto& value : bgra)
        {
            value = std::byte{static_cast<std::uint8_t>(
                random.Next() & 0xFFu)};
        }
        const auto encoded = EncodePngFrame(bgra, 1920, 1080);
        REQUIRE(encoded);
        std::vector<std::byte> decoded(bgra.size());
        const auto status = DecodePngFrame(
            std::span<const std::byte>(encoded.Value()), 1920, 1080,
            std::span<std::byte>(decoded));
        REQUIRE(status);
        CHECK(decoded == bgra);
    }

    // Canonical reference frame (quantized levels compress well).
    {
        const std::vector<std::byte> frame =
            EncodeFrameOrDie(MakeCanonicalGoldenPayload().MakeInput());
        const auto encoded = EncodePngFrame(frame, 1920, 1080);
        REQUIRE(encoded);
        std::vector<std::byte> decoded(frame.size());
        const auto status = DecodePngFrame(
            std::span<const std::byte>(encoded.Value()), 1920, 1080,
            std::span<std::byte>(decoded));
        REQUIRE(status);
        CHECK(decoded == frame);
    }
}

TEST_CASE("PNG encode validates dimensions and input size",
    "[pbmodulation][png][encode-validation]")
{
    const std::array<std::byte, 4> pixels{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    CHECK_FALSE(EncodePngFrame(pixels, 0, 1));
    CHECK_FALSE(EncodePngFrame(pixels, 1, 0));
    CHECK_FALSE(EncodePngFrame(pixels, kMaximumFrameDimension + 1, 1));
    CHECK_FALSE(EncodePngFrame(pixels, 1, kMaximumFrameDimension + 1));
    {
        const auto result = EncodePngFrame(pixels, 16384, 16384);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }
    {
        std::array<std::byte, 8> tooMany{};
        const auto result = EncodePngFrame(tooMany, 1, 1);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }
}
TEST_CASE("Oracle-built PNGs decode to the exact expected pixels",
    "[pbmodulation][png][oracle][decode]")
{
    const std::uint32_t width = 9;
    const std::uint32_t height = 7;

    // 8-bit RGBA with every scanline filter (0-4).
    for (int filterMode = 0; filterMode < 5; filterMode++)
    {
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height,
            pngoracle::PngFormat{},
            filterMode,
            MakeRgbaProvider());
        const std::string filterLabel =
            "rgba-filter-" + std::to_string(filterMode);
        VerifyOracleDecode(filterLabel.c_str(), png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                return {pixel[2], pixel[1], pixel[0], pixel[3]};
            });
    }

    // 8-bit RGB (no alpha): the decoder must add the frozen 0xFF filler.
    {
        pngoracle::PngFormat format;
        format.colorType = 2;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 2,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                pixelBytes[0] = pixel[0];
                pixelBytes[1] = pixel[1];
                pixelBytes[2] = pixel[2];
            });
        VerifyOracleDecode("rgb8", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                return {pixel[2], pixel[1], pixel[0], 255};
            });
    }

    // 8-bit grayscale: B = G = R, alpha filler 255.
    {
        pngoracle::PngFormat format;
        format.colorType = 0;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 3, MakeGrayProvider());
        VerifyOracleDecode("gray8", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t value = PatternGray(x, y);
                return {value, value, value, 255};
            });
    }

    // 8-bit gray + alpha: alpha channel preserved.
    {
        pngoracle::PngFormat format;
        format.colorType = 4;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 4,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                pixelBytes[0] = PatternGray(x, y);
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                pixelBytes[1] = pixel[3];
            });
        VerifyOracleDecode("gray-alpha8", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t value = PatternGray(x, y);
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                return {value, value, value, pixel[3]};
            });
    }

    // 8-bit palette with tRNS (entry 2 is semi-transparent and the
    // provider actually selects indices 0..4, which include it).
    {
        pngoracle::PngFormat format;
        format.colorType = 3;
        // PLTE is one RGB triple per entry (5 entries, entry i is
        // gray 10*(i+1)).
        format.palette = {10, 10, 10, 20, 20, 20, 30, 30, 30,
            40, 40, 40, 50, 50, 50};
        format.tRns = {255, 255, 128, 255, 255};
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 0,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                pixelBytes[0] =
                    static_cast<std::uint8_t>(
                        PatternGray(x, y) % 5);
            });
        VerifyOracleDecode("palette8-trns", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint32_t index =
                    PatternGray(x, y) % 5u;
                const std::uint8_t red =
                    static_cast<std::uint8_t>(
                        10u * (index + 1u));
                const std::uint8_t alpha = static_cast<std::uint8_t>(
                    index == 2u ? 128u : 255u);
                return {red, red, red, alpha};
            });
    }

    // 4-bit palette (16 entries, no tRNS).
    {
        pngoracle::PngFormat format;
        format.bitDepth = 4;
        format.colorType = 3;
        // PLTE is one RGB triple per entry (16 entries, entry i is
        // gray 16*i).
        format.palette = {0, 0, 0, 16, 16, 16, 32, 32, 32,
            48, 48, 48, 64, 64, 64, 80, 80, 80, 96, 96, 96,
            112, 112, 112, 128, 128, 128, 144, 144, 144,
            160, 160, 160, 176, 176, 176, 192, 192, 192,
            208, 208, 208, 224, 224, 224, 240, 240, 240};
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 1,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                pixelBytes[0] =
                    static_cast<std::uint8_t>(
                        PatternGray(x, y) & 0x0Fu);
            });
        VerifyOracleDecode("palette4", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t value =
                    static_cast<std::uint8_t>(
                        (PatternGray(x, y) & 0x0Fu) * 16u);
                return {value, value, value, 255};
            });
    }

    // 16-bit RGB: the decoder strips to the high byte.
    {
        pngoracle::PngFormat format;
        format.bitDepth = 16;
        format.colorType = 2;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 3,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                for (std::size_t channel = 0; channel < 3;
                    channel++)
                {
                    pixelBytes[channel * 2] =
                        pixel[channel]; // high byte
                    pixelBytes[channel * 2 + 1] = 0x11; // low
                }
            });
        VerifyOracleDecode("rgb16", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                return {pixel[2], pixel[1], pixel[0], 255};
            });
    }

    // 16-bit gray: stripped to the high byte.
    {
        pngoracle::PngFormat format;
        format.bitDepth = 16;
        format.colorType = 0;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 0,
            [](const std::uint32_t x, const std::uint32_t y,
                std::uint8_t* pixelBytes)
            {
                pixelBytes[0] = PatternGray(x, y);
                pixelBytes[1] = 0x7F;
            });
        VerifyOracleDecode("gray16", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t value = PatternGray(x, y);
                return {value, value, value, 255};
            });
    }

    // 1/2/4-bit grayscale: libpng expands samples to the full 8-bit
    // range (value * 255 / maxSample, exact for these depths).
    const std::vector<std::pair<int, std::uint32_t>> subByteDepths =
        {{1, 1}, {2, 3}, {4, 15}};
    for (const auto& [bitDepth, maxSample] : subByteDepths)
    {
        pngoracle::PngFormat format;
        format.bitDepth = bitDepth;
        format.colorType = 0;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 0,
            [&maxSample](const std::uint32_t x,
                const std::uint32_t y, std::uint8_t* pixelBytes)
            {
                pixelBytes[0] =
                    static_cast<std::uint8_t>(
                        PatternGray(x, y) % (maxSample + 1u));
            });
        const std::string subByteLabel =
            "gray-subbyte-" + std::to_string(maxSample);
        VerifyOracleDecode(subByteLabel.c_str(), png, width, height,
            [&maxSample](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t sample =
                    static_cast<std::uint8_t>(
                        PatternGray(x, y) % (maxSample + 1u));
                const std::uint8_t expanded =
                    static_cast<std::uint8_t>(
                        sample * (255u / maxSample));
                return {expanded, expanded, expanded, 255};
            });
    }

    // Interlaced (Adam7) 8-bit RGBA and gray.
    {
        pngoracle::PngFormat format;
        format.interlaced = true;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            width, height, format, 0, MakeRgbaProvider());
        VerifyOracleDecode("interlaced-rgba8", png, width, height,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::array<std::uint8_t, 4> pixel =
                    PatternRgba(x, y);
                return {pixel[2], pixel[1], pixel[0], pixel[3]};
            });
    }
    {
        pngoracle::PngFormat format;
        format.colorType = 0;
        format.interlaced = true;
        const std::vector<std::byte> png = pngoracle::BuildPng(
            7, 9, format, 4, MakeGrayProvider());
        VerifyOracleDecode("interlaced-gray8", png, 7, 9,
            [](const std::uint32_t x,
                const std::uint32_t y) -> std::array<std::uint8_t, 4>
            {
                const std::uint8_t value = PatternGray(x, y);
                return {value, value, value, 255};
            });
    }
}
TEST_CASE("Malformed PNG streams fail closed",
    "[pbmodulation][png][malformed]")
{
    const std::vector<std::byte> validPng = MakeSmallValidPng();
    const std::uint32_t width = 8;
    const std::uint32_t height = 5;
    std::vector<std::byte> out(width * height * 4u);

    const auto checkMalformed =
        [&validPng, &width, &height, &out](
            const std::vector<std::byte>& png,
            const ModulationErrorCode expectedCode)
    {
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        CHECK(status.Error().code == expectedCode);
    };

    // Signature: corrupt each of the 8 bytes.
    for (std::size_t signatureByte = 0; signatureByte < 8;
        signatureByte++)
    {
        std::vector<std::byte> png = validPng;
        png[signatureByte] =
            std::byte{static_cast<std::uint8_t>(
                std::to_integer<std::uint8_t>(png[signatureByte])
                ^ 0xFFu)};
        checkMalformed(png, ModulationErrorCode::PngDecodeError);
    }

    // Truncated signature.
    {
        const std::vector<std::byte> png =
            std::vector<std::byte>(validPng.begin(),
                validPng.begin() + 7);
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::TruncatedInput);
        CHECK(status.Error().offset == 7);
    }

    // IDAT truncated mid-stream (IEND and part of IDAT removed).
    {
        std::vector<std::byte> png = validPng;
        png.resize(png.size() - 25);
        checkMalformed(png, ModulationErrorCode::PngDecodeError);
    }

    // IEND removed entirely: the strict reference stream must end
    // exactly at IEND; an incomplete stream is rejected. Every pixel is
    // decodable before the stream-end check fails, so this case pins the
    // contract that even a post-decode failure leaves outBgra untouched.
    {
        std::vector<std::byte> png = validPng;
        png.resize(png.size() - 12);
        std::fill(out.begin(), out.end(), std::byte{0x5A});
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::PngDecodeError);
        for (const auto value : out)
        {
            CHECK(value == std::byte{0x5A});
        }
    }

    // Garbage after IEND: the pixels decode fully and png_read_end
    // succeeds, so only the strict stream-end check fails; outBgra must
    // still be untouched.
    {
        std::vector<std::byte> png = validPng;
        png.insert(png.end(), {std::byte{1}, std::byte{2},
            std::byte{3}});
        std::fill(out.begin(), out.end(), std::byte{0x5A});
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::TrailingBytes);
        CHECK(status.Error().offset == png.size() - 3);
        for (const auto value : out)
        {
            CHECK(value == std::byte{0x5A});
        }
    }

    // IHDR chunk CRC corrupted.
    {
        std::vector<std::byte> png = validPng;
        png[29] = std::byte{static_cast<std::uint8_t>(
            std::to_integer<std::uint8_t>(png[29]) ^ 0xFFu)};
        checkMalformed(png, ModulationErrorCode::PngDecodeError);
    }

    // Invalid color type with a corrected chunk CRC (libpng must reject
    // the IHDR content itself, not just the CRC).
    {
        std::vector<std::byte> png = validPng;
        png[25] = std::byte{7};
        std::vector<std::byte> crcInput;
        crcInput.insert(crcInput.end(), png.begin() + 12,
            png.begin() + 29);
        const std::uint32_t crc =
            pngoracle::Crc32(std::span<const std::byte>(crcInput));
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            png[29 + (24 - shift) / 8] = std::byte{
                static_cast<std::uint8_t>((crc >> shift) & 0xFFu)};
        }
        checkMalformed(png, ModulationErrorCode::PngDecodeError);
    }

    // Invalid bit depth with a corrected chunk CRC.
    {
        std::vector<std::byte> png = validPng;
        png[24] = std::byte{7};
        std::vector<std::byte> crcInput;
        crcInput.insert(crcInput.end(), png.begin() + 12,
            png.begin() + 29);
        const std::uint32_t crc =
            pngoracle::Crc32(std::span<const std::byte>(crcInput));
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            png[29 + (24 - shift) / 8] = std::byte{
                static_cast<std::uint8_t>((crc >> shift) & 0xFFu)};
        }
        checkMalformed(png, ModulationErrorCode::PngDecodeError);
    }

    // Decoded dimensions differ from the expectation.
    {
        const auto status = DecodePngFrame(
            std::span<const std::byte>(validPng), 2, 2,
            std::span<std::byte>(out).first(16));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::FrameGeometryMismatch);
    }

    // Output buffer size mismatch.
    {
        const auto status = DecodePngFrame(
            std::span<const std::byte>(validPng), width, height,
            std::span<std::byte>(out).first(out.size() - 4));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }

    // The decoder never mutates the output buffer on failure.
    {
        std::vector<std::byte> png = validPng;
        png[0] = std::byte{0x00};
        std::fill(out.begin(), out.end(), std::byte{0x5A});
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        for (const auto value : out)
        {
            CHECK(value == std::byte{0x5A});
        }
    }

    // Mid-stream failure must leave the output untouched too: the IDAT
    // is truncated after rows 0..3 are decodable (13 deflate payload
    // bytes removed from the 234-byte 8x5 fixture), so a decoder that
    // converted rows into outBgra as it read them would have written
    // four partial rows before the stream ran out.
    {
        std::vector<std::byte> png = validPng;
        png.resize(png.size() - 25);
        std::fill(out.begin(), out.end(), std::byte{0x5A});
        const auto status = DecodePngFrame(
            std::span<const std::byte>(png), width, height,
            std::span<std::byte>(out));
        CHECK_FALSE(status);
        for (const auto value : out)
        {
            CHECK(value == std::byte{0x5A});
        }
    }
}
