#pragma once

#include "pbmodulation/desktop_levels.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace desktoptest
{
inline std::vector<std::byte> Load(const std::string& name, const std::size_t size)
{
    const auto path = std::filesystem::path(PB_DESKTOP_LEVELS_GOLDEN_DIR) / name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() != static_cast<std::streamoff>(size))
    {
        throw std::runtime_error("Wrong-size or missing DesktopLevels Golden: " + path.string());
    }
    stream.seekg(0);
    std::vector<std::byte> bytes(size);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
    {
        throw std::runtime_error("Cannot read DesktopLevels Golden");
    }
    return bytes;
}

inline std::string Stem(const unsigned tile, const unsigned phase)
{
    return "tile" + std::to_string(tile) + "-phase" + std::to_string(phase);
}

inline std::vector<std::byte> Record(const unsigned tile, const unsigned phase = 0)
{
    return Load(Stem(tile, phase) + "-record.bin", 44);
}

inline std::vector<std::byte> Data(const unsigned tile, const unsigned phase = 0)
{
    const std::size_t capacity = tile == 2 ? 86688 : 21672;
    std::vector<std::byte> bytes(capacity);
    for (std::size_t index = 0; index < capacity / 2025 * 2025; index++)
    {
        bytes[index] = static_cast<std::byte>((index * 37 + phase * 11 + 5) & 255);
    }
    return bytes;
}

inline void Paint(std::span<std::byte> pixels, const std::uint32_t x, const std::uint32_t y, const std::uint32_t width,
                  const std::uint32_t height, const unsigned level)
{
    if (x > 1920 || y > 1080 || width > 1920 - x || height > 1080 - y || pixels.size() != 1920 * 1080 * 4)
    {
        throw std::runtime_error("Invalid independent painter rectangle");
    }
    for (std::uint32_t row = y; row < y + height; row++)
    {
        for (std::uint32_t column = x; column < x + width; column++)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * 1920 + column) * 4;
            pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

inline std::vector<std::array<std::uint32_t, 2>> Coordinates(const unsigned tile)
{
    std::vector<std::array<std::uint32_t, 2>> positions;
    for (std::uint32_t y = 96; y < 984; y += tile)
    {
        for (std::uint32_t x = 96; x < 1824; x += tile)
        {
            const bool timingRow = (y >= 160 && y < 288) || (y >= 476 && y < 604) || (y >= 792 && y < 920);
            const bool timingColumn = x < 224 || (x >= 896 && x < 1024) || x >= 1696;
            if (!timingRow || !timingColumn)
            {
                positions.push_back({x, y});
            }
        }
    }
    return positions;
}

// Independent literal painter: no production layout, encoder, interleave,
// timing, RS or centroid functions determine these expected pixels.
inline std::vector<std::byte> Raster(const unsigned tile, const unsigned phase = 0)
{
    std::vector<std::byte> pixels(1920 * 1080 * 4);
    Paint(pixels, 0, 0, 1920, 1080, 128);
    const auto markers = Load("marker-bits.bin", 196);
    constexpr std::array<std::array<unsigned, 2>, 4> corners{{{16, 16}, {1840, 16}, {16, 1000}, {1840, 1000}}};
    for (std::size_t index = 0; index < 4; index++)
    {
        const auto position = corners[index];
        Paint(pixels, position[0], position[1], 64, 64, 224);
        for (unsigned cell = 0; cell < 49; cell++)
        {
            Paint(pixels, position[0] + 4 + (cell % 7) * 8, position[1] + 4 + (cell / 7) * 8, 8, 8,
                  markers[index * 49 + cell] == std::byte{0} ? 32 : 224);
        }
    }
    const auto codeword = Load(Stem(tile, phase) + "-rs76.bin", 76);
    for (const auto position : std::array<std::array<unsigned, 2>, 2>{{{96, 16}, {1216, 1000}}})
    {
        for (unsigned bit = 0; bit < 608; bit++)
        {
            Paint(pixels, position[0] + (bit % 76) * 8, position[1] + (bit / 76) * 8, 8, 8,
                ((std::to_integer<unsigned>(codeword[bit / 8]) >> (bit % 8)) & 1) != 0 ? 224 : 32);
        }
    }
    const auto timing = Load(Stem(tile, phase) + "-timing.bin", 2304);
    unsigned timingIndex = 0;
    for (const unsigned y : {160u, 476u, 792u})
    {
        for (const unsigned x : {96u, 896u, 1696u})
        {
            for (unsigned cell = 0; cell < 256; cell++)
            {
                Paint(pixels, x + (cell % 16) * 8, y + (cell / 16) * 8, 8, 8, timing[timingIndex * 256 + cell] == std::byte{0} ? 32 : 224);
            }
            timingIndex++;
        }
    }
    for (const auto position : std::array<std::array<unsigned, 2>, 4>{{{736, 16}, {1696, 16}, {96, 1000}, {1056, 1000}}})
    {
        for (unsigned index = 0; index < 4; index++)
        {
            Paint(pixels, position[0] + 32 * index, position[1], 32, 64, 32 + 64 * index);
        }
    }
    for (const unsigned y : {16u, 1000u})
    {
        for (unsigned row = 0; row < 64; row++)
        {
            for (unsigned column = 0; column < 128; column++)
            {
                Paint(pixels, 896 + column, y + row, 1, 1, ((column < 64 ? column : row) % 2) == 0 ? 32 : 224);
            }
        }
    }
    const auto data = Data(tile, phase);
    const auto positions = Coordinates(tile);
    constexpr std::array<unsigned, 4> inverseGray{0, 1, 3, 2};
    for (std::size_t logical = 0; logical < positions.size(); logical++)
    {
        const std::size_t physical = (logical * 65537ull + phase * (1728 / tile)) % positions.size();
        const unsigned label = (std::to_integer<unsigned>(data[logical / 4]) >> (2 * (logical % 4))) & 3;
        Paint(pixels, positions[physical][0], positions[physical][1], tile, tile, 32 + 64 * inverseGray[label]);
    }
    return pixels;
}

inline pbmodulation::LumaView View(const std::span<const std::byte> pixels)
{
    return {pixels, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

inline std::string Hex(const std::span<const std::byte> bytes)
{
    std::string result;
    for (const auto byte : bytes)
    {
        constexpr char alphabet[] = "0123456789abcdef";
        const unsigned value = std::to_integer<unsigned>(byte);
        result.push_back(alphabet[value >> 4]);
        result.push_back(alphabet[value & 15]);
    }
    return result;
}
} // namespace desktoptest
