#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace localdesktoptest
{

// Independent test-only filters: integrate pixel rectangles for Area, or
// evaluate a centre-aligned tent kernel for Bilinear. Neither calls SampleLuma,
// production geometry, raster, RS, marker, or timing helpers.
enum class FixtureFilter
{
    Area, Bilinear
};

struct GrayImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
    std::vector<std::byte> pixels;

    GrayImage(const std::uint32_t imageWidth, const std::uint32_t imageHeight, const std::size_t padding = 13) :
        width(imageWidth), height(imageHeight), pitch(static_cast<std::size_t>(width) + padding)
    {
        if (width == 0 || height == 0 || width > 8192 || height > 8192 || padding > 64)
        {
            throw std::runtime_error("Invalid independent fixture dimensions");
        }
        pixels.resize(pitch * height, std::byte{0xA5});
        for (std::uint32_t row = 0; row < height; row++)
        {
            std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(row * pitch), width, std::byte{128});
        }
    }

    [[nodiscard]] pbmodulation::LumaView View() const noexcept
    {
        return {pixels, width, height, pitch, pbmodulation::LumaPixelFormat::Gray8};
    }

    [[nodiscard]] std::uint8_t Read(const std::int64_t x, const std::int64_t y) const noexcept
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
        {
            return 128;
        }
        return std::to_integer<std::uint8_t>(pixels[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x)]);
    }

    void Write(const std::uint32_t x, const std::uint32_t y, const std::uint8_t value)
    {
        if (x >= width || y >= height)
        {
            throw std::runtime_error("Fixture write outside declared pixels");
        }
        pixels[static_cast<std::size_t>(y) * pitch + x] = static_cast<std::byte>(value);
    }
};

[[nodiscard]] inline GrayImage GrayFromGolden(const std::span<const std::byte> bgra)
{
    if (bgra.size() != std::size_t{1920} * 1080 * 4)
    {
        throw std::runtime_error("Wrong independent raster size");
    }
    GrayImage result(1920, 1080);
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            const std::size_t offset = (std::size_t{row} * 1920 + column) * 4;
            if (bgra[offset] != bgra[offset + 1] || bgra[offset] != bgra[offset + 2] || bgra[offset + 3] != std::byte{255})
            {
                throw std::runtime_error("Golden must be neutral, opaque SDR");
            }
            result.Write(column, row, std::to_integer<std::uint8_t>(bgra[offset]));
        }
    }
    return result;
}

inline void PaintBlock(GrayImage& image, const std::uint32_t x, const std::uint32_t y,
                       const std::uint32_t width, const std::uint32_t height, const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < height; row++)
    {
        for (std::uint32_t column = 0; column < width; column++)
        {
            image.Write(x + column, y + row, level);
        }
    }
}

inline void CopyBlock(const GrayImage& source, GrayImage& destination, const std::uint32_t sourceX, const std::uint32_t sourceY,
                      const std::uint32_t width, const std::uint32_t height, const std::uint32_t destinationX, const std::uint32_t destinationY)
{
    for (std::uint32_t row = 0; row < height; row++)
    {
        for (std::uint32_t column = 0; column < width; column++)
        {
            destination.Write(destinationX + column, destinationY + row, source.Read(sourceX + column, sourceY + row));
        }
    }
}

[[nodiscard]] inline double AreaValue(const GrayImage& source, const double left, const double top, const double right, const double bottom)
{
    double integral = 0;
    const auto firstX = static_cast<std::int64_t>(std::floor(left));
    const auto firstY = static_cast<std::int64_t>(std::floor(top));
    const auto lastX = static_cast<std::int64_t>(std::ceil(right));
    const auto lastY = static_cast<std::int64_t>(std::ceil(bottom));
    for (std::int64_t row = firstY; row < lastY; row++)
    {
        const double height = std::max(0.0, std::min(bottom, static_cast<double>(row + 1)) - std::max(top, static_cast<double>(row)));
        for (std::int64_t column = firstX; column < lastX; column++)
        {
            const double width = std::max(0.0, std::min(right, static_cast<double>(column + 1)) - std::max(left, static_cast<double>(column)));
            integral += width * height * source.Read(column, row);
        }
    }
    return integral / ((right - left) * (bottom - top));
}

[[nodiscard]] inline double TentValue(const GrayImage& source, const double x, const double y)
{
    const auto left = static_cast<std::int64_t>(std::floor(x));
    const auto top = static_cast<std::int64_t>(std::floor(y));
    double value = 0;
    for (std::int64_t row = top; row <= top + 1; row++)
    {
        for (std::int64_t column = left; column <= left + 1; column++)
        {
            value += (1 - std::abs(x - static_cast<double>(column))) * (1 - std::abs(y - static_cast<double>(row))) * source.Read(column, row);
        }
    }
    return value;
}

[[nodiscard]] inline GrayImage Resample(const GrayImage& source, const double scaleX, const double scaleY,
                                       const double originX, const double originY, const FixtureFilter filter, const std::uint32_t trailing = 12)
{
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX < 0.4 || scaleX > 2.1 || scaleY < 0.4 || scaleY > 2.1 ||
        originX < 0 || originY < 0 || originX > 100 || originY > 100)
    {
        throw std::runtime_error("Invalid independent resampling parameters");
    }
    GrayImage result(static_cast<std::uint32_t>(std::ceil(originX + source.width * scaleX + trailing)),
                     static_cast<std::uint32_t>(std::ceil(originY + source.height * scaleY + trailing)));
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            const double value = filter == FixtureFilter::Area ?
                AreaValue(source, (column - originX) / scaleX, (row - originY) / scaleY,
                          (column + 1.0 - originX) / scaleX, (row + 1.0 - originY) / scaleY) :
                TentValue(source, (column + 0.5 - originX) / scaleX - 0.5, (row + 0.5 - originY) / scaleY - 0.5);
            result.Write(column, row, static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0))));
        }
    }
    return result;
}

[[nodiscard]] inline GrayImage Blend(const GrayImage& first, const GrayImage& second, const unsigned secondQuarters)
{
    if (first.width != second.width || first.height != second.height || secondQuarters > 4)
    {
        throw std::runtime_error("Mismatched independent blend dimensions");
    }
    GrayImage result(first.width, first.height);
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            const unsigned value = (4 - secondQuarters) * first.Read(column, row) + secondQuarters * second.Read(column, row);
            result.Write(column, row, static_cast<std::uint8_t>((value + 2) / 4));
        }
    }
    return result;
}

inline void PaintCopy(GrayImage& image, const std::size_t copy, const std::span<const std::byte> codeword)
{
    if (copy > 1 || codeword.size() != 76)
    {
        throw std::runtime_error("Bad independent codeword fixture");
    }
    const std::uint32_t originX = copy == 0 ? 96u : 1216u;
    const std::uint32_t originY = copy == 0 ? 16u : 1000u;
    for (std::uint32_t bit = 0; bit < 608; bit++)
    {
        const auto value = (std::to_integer<std::uint8_t>(codeword[bit / 8]) >> (bit % 8)) & 1u;
        PaintBlock(image, originX + (bit % 76) * 8, originY + (bit / 76) * 8, 8, 8, value != 0 ? 224 : 32);
    }
}

inline void PaintTimingCell(GrayImage& image, const std::uint32_t patch, const std::uint32_t cell, const std::uint8_t value)
{
    constexpr std::array<std::uint32_t, 3> originsX{96, 896, 1696};
    constexpr std::array<std::uint32_t, 3> originsY{160, 476, 792};
    if (patch >= 9 || cell >= 256)
    {
        throw std::runtime_error("Bad independent timing cell");
    }
    PaintBlock(image, originsX[patch % 3] + (cell % 16) * 8, originsY[patch / 3] + (cell / 16) * 8, 8, 8, value);
}

[[nodiscard]] inline std::uint16_t LinearHalfForByte(const unsigned value)
{
    const double encoded = static_cast<double>(value) / 255;
    const double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    const auto valueOf = [](const std::uint16_t half)
    {
        const auto exponent = static_cast<unsigned>(half >> 10);
        return exponent == 0 ? static_cast<double>(half) / 16777216.0 :
                               std::ldexp(static_cast<double>(1024u + (half & 1023u)), static_cast<int>(exponent) - 25);
    };
    std::uint16_t lower = 0;
    std::uint16_t upper = 0x3C00;
    while (static_cast<unsigned>(upper - lower) > 1)
    {
        const auto middle = static_cast<std::uint16_t>((static_cast<unsigned>(lower) + upper) / 2);
        if (valueOf(middle) <= linear)
        {
            lower = middle;
        }
        else
        {
            upper = middle;
        }
    }
    return linear - valueOf(lower) <= valueOf(upper) - linear ? lower : upper;
}

struct EncodedFixture
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
    pbmodulation::LumaPixelFormat format = pbmodulation::LumaPixelFormat::Gray8;
    std::vector<std::byte> pixels;

    [[nodiscard]] pbmodulation::LumaView View() const noexcept
    {
        return {pixels, width, height, pitch, format};
    }
};

[[nodiscard]] inline EncodedFixture ConvertFormat(const GrayImage& source, const pbmodulation::LumaPixelFormat format)
{
    const std::size_t pixelBytes = format == pbmodulation::LumaPixelFormat::Fp16LinearSdr ? 8 : 4;
    EncodedFixture result{source.width, source.height, source.width * pixelBytes + 17, format, {}};
    result.pixels.resize(result.pitch * result.height, std::byte{0xDA});
    std::array<std::uint16_t, 256> halves{};
    if (format == pbmodulation::LumaPixelFormat::Fp16LinearSdr)
    {
        for (unsigned value = 0; value < halves.size(); value++)
        {
            halves[value] = LinearHalfForByte(value);
        }
    }
    for (std::uint32_t row = 0; row < source.height; row++)
    {
        for (std::uint32_t column = 0; column < source.width; column++)
        {
            const auto value = source.Read(column, row);
            auto* destination = result.pixels.data() + row * result.pitch + column * pixelBytes;
            if (format == pbmodulation::LumaPixelFormat::Bgra8)
            {
                destination[0] = destination[1] = destination[2] = static_cast<std::byte>(value);
                destination[3] = std::byte{17}; // Alpha is not a luma transport channel.
            }
            else if (format == pbmodulation::LumaPixelFormat::R10G10B10A2)
            {
                const std::uint32_t channel = (static_cast<std::uint32_t>(value) * 1023 + 127) / 255;
                const std::uint32_t packed = channel | (channel << 10) | (channel << 20) | 0xC0000000u;
                for (unsigned byte = 0; byte < 4; byte++)
                {
                    destination[byte] = static_cast<std::byte>((packed >> (byte * 8)) & 255u);
                }
            }
            else if (format == pbmodulation::LumaPixelFormat::Fp16LinearSdr)
            {
                for (unsigned component = 0; component < 4; component++)
                {
                    const auto half = component == 3 ? std::uint16_t{0x3C00} : halves[value];
                    destination[component * 2] = static_cast<std::byte>(half & 255u);
                    destination[component * 2 + 1] = static_cast<std::byte>(half >> 8);
                }
            }
            else
            {
                throw std::runtime_error("Unexpected test conversion format");
            }
        }
    }
    return result;
}

} // namespace localdesktoptest
