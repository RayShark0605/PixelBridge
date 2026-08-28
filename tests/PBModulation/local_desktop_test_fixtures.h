#pragma once

// A literal second region painter consuming independent Python-oracle fixtures.
// It must not call production Encode, MarkerModule, RS, timing, or layout helpers.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace localdesktoptest
{

[[nodiscard]] inline std::vector<std::byte> LoadGoldenBytes(const std::string_view filename, const std::size_t expectedBytes)
{
    if (expectedBytes > 64 * 1024 || filename.empty() || filename.find_first_of("/\\:") != std::string_view::npos)
    {
        throw std::runtime_error("Invalid bounded Golden fixture request");
    }
    const auto path = std::filesystem::path(PB_LOCAL_DESKTOP_GOLDEN_DIR) / std::string(filename);
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() != static_cast<std::streamoff>(expectedBytes))
    {
        throw std::runtime_error("Missing or wrong-size Golden file: " + path.string());
    }
    stream.seekg(0);
    std::vector<std::byte> result(expectedBytes);
    if (!stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size())))
    {
        throw std::runtime_error("Cannot read Golden file: " + path.string());
    }
    return result;
}

[[nodiscard]] inline std::array<std::byte, 44> LoadGoldenRecord(const std::string_view stem = "a")
{
    const auto bytes = LoadGoldenBytes(std::string(stem) + "-record.bin", 44);
    std::array<std::byte, 44> record{};
    std::copy(bytes.begin(), bytes.end(), record.begin());
    return record;
}

[[nodiscard]] inline std::vector<std::byte> MakeGoldenRaster(const std::string_view stem = "a")
{
    const auto markers = LoadGoldenBytes("marker-bits.bin", 196);
    const auto codeword = LoadGoldenBytes(std::string(stem) + "-rs76.bin", 76);
    const auto timing = LoadGoldenBytes(std::string(stem) + "-timing-bits.bin", 2304);
    std::vector<std::byte> pixels(std::size_t{1920} * 1080 * 4);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        pixels[offset] = std::byte{128};
        pixels[offset + 1] = std::byte{128};
        pixels[offset + 2] = std::byte{128};
        pixels[offset + 3] = std::byte{255};
    }
    const auto fill = [&pixels](const std::uint32_t x, const std::uint32_t y, const std::uint32_t width,
                                const std::uint32_t height, const std::uint8_t level)
    {
        if (x > 1920 || y > 1080 || width > 1920 - x || height > 1080 - y)
        {
            throw std::runtime_error("Independent fixture rectangle outside canvas");
        }
        for (std::uint32_t row = y; row < y + height; row++)
        {
            for (std::uint32_t column = x; column < x + width; column++)
            {
                const std::size_t offset = (std::size_t{row} * 1920 + column) * 4;
                pixels[offset] = static_cast<std::byte>(level);
                pixels[offset + 1] = static_cast<std::byte>(level);
                pixels[offset + 2] = static_cast<std::byte>(level);
                pixels[offset + 3] = std::byte{255};
            }
        }
    };
    constexpr std::array<std::array<std::uint32_t, 2>, 4> markerOrigins{{{16, 16}, {1840, 16}, {16, 1000}, {1840, 1000}}};
    for (std::size_t markerIndex = 0; markerIndex < markerOrigins.size(); markerIndex++)
    {
        const auto& origin = markerOrigins[markerIndex];
        fill(origin[0], origin[1], 64, 64, 224);
        for (std::uint32_t cell = 0; cell < 49; cell++)
        {
            const auto value = std::to_integer<std::uint8_t>(markers[markerIndex * 49 + cell]);
            if (value > 1)
            {
                throw std::runtime_error("Nonbinary marker oracle");
            }
            fill(origin[0] + 4 + (cell % 7) * 8, origin[1] + 4 + (cell / 7) * 8, 8, 8, value != 0 ? 224 : 32);
        }
    }
    constexpr std::array<std::array<std::uint32_t, 2>, 2> copyOrigins{{{96, 16}, {1216, 1000}}};
    for (const auto& origin : copyOrigins)
    {
        for (std::uint32_t bit = 0; bit < 608; bit++)
        {
            const auto symbol = std::to_integer<std::uint8_t>(codeword[bit / 8]);
            fill(origin[0] + (bit % 76) * 8, origin[1] + (bit / 76) * 8, 8, 8, ((symbol >> (bit % 8)) & 1u) != 0 ? 224 : 32);
        }
    }
    constexpr std::array<std::uint32_t, 3> timingX{96, 896, 1696};
    constexpr std::array<std::uint32_t, 3> timingY{160, 476, 792};
    for (std::uint32_t pilot = 0; pilot < 9; pilot++)
    {
        for (std::uint32_t cell = 0; cell < 256; cell++)
        {
            const auto value = std::to_integer<std::uint8_t>(timing[pilot * 256 + cell]);
            if (value > 1)
            {
                throw std::runtime_error("Nonbinary timing oracle");
            }
            fill(timingX[pilot % 3] + (cell % 16) * 8, timingY[pilot / 3] + (cell / 16) * 8, 8, 8, value != 0 ? 224 : 32);
        }
    }
    return pixels;
}

[[nodiscard]] inline std::string ToHex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

} // namespace localdesktoptest
