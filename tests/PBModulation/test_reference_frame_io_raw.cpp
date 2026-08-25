#include "modulation_test_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;
using pbmodulation::ModulationErrorCode;

namespace {

// Independent construction of the frozen PBRW layout (header bytes are
// written by hand, mirroring the documented offsets) so the malformed
// matrix does not rely on the library's own encoder.
std::vector<std::byte> BuildRawContainer(
    const std::span<const std::byte> pixels,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::byte> raw(kRawFrameHeaderBytes + pixels.size());
    for (std::size_t i = 0; i < 4; i++)
    {
        raw[i] = kRawFrameMagic[i];
    }
    raw[4] = std::byte{kRawFrameVersion};
    // reserved bytes 5..7 remain zero.
    const auto writeUint32Le = [&raw](
        const std::size_t offset, const std::uint32_t value)
    {
        raw[offset] = std::byte{value & 0xFFu};
        raw[offset + 1] = std::byte{(value >> 8) & 0xFFu};
        raw[offset + 2] = std::byte{(value >> 16) & 0xFFu};
        raw[offset + 3] = std::byte{(value >> 24) & 0xFFu};
    };
    writeUint32Le(8, width);
    writeUint32Le(12, height);
    writeUint32Le(16, static_cast<std::uint32_t>(pixels.size()));
    // reserved bytes 20..27 remain zero.
    std::copy(pixels.begin(), pixels.end(),
        raw.begin() + static_cast<std::ptrdiff_t>(kRawFrameHeaderBytes));
    return raw;
}

} // namespace

TEST_CASE("PBRW header uses the documented byte layout",
    "[pbmodulation][raw][layout]")
{
    const std::array<std::byte, 4> pixels{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto encoded = EncodeRawFrame(pixels, 1, 1);
    REQUIRE(encoded);
    const std::vector<std::byte>& raw = encoded.Value();
    REQUIRE(raw.size() == kRawFrameHeaderBytes + 4);
    for (std::size_t i = 0; i < 4; i++)
    {
        CHECK(raw[i] == kRawFrameMagic[i]);
    }
    CHECK(raw[4] == std::byte{kRawFrameVersion});
    for (std::size_t i = 5; i < 8; i++)
    {
        CHECK(raw[i] == std::byte{0});
    }
    CHECK(raw[8] == std::byte{1});
    CHECK(raw[9] == std::byte{0});
    CHECK(raw[10] == std::byte{0});
    CHECK(raw[11] == std::byte{0});
    CHECK(raw[12] == std::byte{1});
    CHECK(raw[13] == std::byte{0});
    CHECK(raw[14] == std::byte{0});
    CHECK(raw[15] == std::byte{0});
    CHECK(raw[16] == std::byte{4});
    CHECK(raw[17] == std::byte{0});
    CHECK(raw[18] == std::byte{0});
    CHECK(raw[19] == std::byte{0});
    for (std::size_t i = 20; i < 28; i++)
    {
        CHECK(raw[i] == std::byte{0});
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        CHECK(raw[28 + i] == pixels[i]);
    }
}

TEST_CASE("PBRW round-trips small and full-size frames",
    "[pbmodulation][raw][roundtrip]")
{
    // 1x1.
    {
        const std::array<std::byte, 4> pixels{
            std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
            std::byte{0x44}};
        const auto encoded = EncodeRawFrame(pixels, 1, 1);
        REQUIRE(encoded);
        std::array<std::byte, 4> decoded{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(encoded.Value()),
            std::span<std::byte>(decoded), width, height);
        REQUIRE(status);
        CHECK(width == 1);
        CHECK(height == 1);
        CHECK(decoded == pixels);
    }

    // 64x64 random.
    {
        std::vector<std::byte> pixels(64u * 64u * 4u);
        SplitMix64 random(0x5EED1234u);
        for (auto& value : pixels)
        {
            value = std::byte{static_cast<std::uint8_t>(
                random.Next() & 0xFFu)};
        }
        const auto encoded = EncodeRawFrame(pixels, 64, 64);
        REQUIRE(encoded);
        std::vector<std::byte> decoded(pixels.size());
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(encoded.Value()),
            std::span<std::byte>(decoded), width, height);
        REQUIRE(status);
        CHECK(width == 64);
        CHECK(height == 64);
        CHECK(decoded == pixels);
    }

    // Full canonical reference frame.
    {
        const std::vector<std::byte> frame =
            EncodeFrameOrDie(MakeCanonicalGoldenPayload().MakeInput());
        const auto encoded = EncodeRawFrame(frame, 1920, 1080);
        REQUIRE(encoded);
        std::vector<std::byte> decoded(frame.size());
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(encoded.Value()),
            std::span<std::byte>(decoded), width, height);
        REQUIRE(status);
        CHECK(width == 1920);
        CHECK(height == 1080);
        CHECK(decoded == frame);
    }
}

TEST_CASE("PBRW rejects malformed containers",
    "[pbmodulation][raw][malformed]")
{
    const std::array<std::byte, 16> pixels{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
        std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
        std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}};
    std::array<std::byte, 16> out{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Truncated header / empty input.
    CHECK_FALSE(DecodeRawFrame(
        std::span<const std::byte>{}, out, width, height));
    {
        const std::vector<std::byte> headerOnly(27, std::byte{0});
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(headerOnly), out, width, height);
        CHECK_FALSE(status);
        CHECK(status.Error().code == ModulationErrorCode::TruncatedInput);
        CHECK(status.Error().offset == 27);
    }

    const auto checkMalformed =
        [&out, &width, &height](
            const std::vector<std::byte>& raw,
            const ModulationErrorCode expectedCode,
            const std::size_t expectedOffset)
    {
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(raw), out, width, height);
        CHECK_FALSE(status);
        CHECK(status.Error().code == expectedCode);
        CHECK(status.Error().offset == expectedOffset);
    };

    // Magic.
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, 2, 2);
        raw[0] = std::byte{0x00};
        checkMalformed(raw, ModulationErrorCode::InvalidMagic, 0);
    }

    // Version.
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, 2, 2);
        raw[4] = std::byte{2};
        checkMalformed(raw, ModulationErrorCode::UnsupportedVersion, 4);
    }

    // Front reserved bytes (one at a time).
    for (std::size_t reservedOffset = 5; reservedOffset < 8;
        reservedOffset++)
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, 2, 2);
        raw[reservedOffset] = std::byte{1};
        checkMalformed(raw,
            ModulationErrorCode::NonZeroReservedByte, 5);
    }

    // Tail reserved bytes (first and last of the 8-byte block).
    for (const std::size_t reservedOffset : {20, 27})
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, 2, 2);
        raw[reservedOffset] = std::byte{1};
        checkMalformed(raw,
            ModulationErrorCode::NonZeroReservedByte, 20);
    }

    // Dimension bounds.
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 0, 2);
        checkMalformed(raw, ModulationErrorCode::InvalidInput, 0);
    }
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 0);
        checkMalformed(raw, ModulationErrorCode::InvalidInput, 0);
    }
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, kMaximumFrameDimension + 1, 2);
        checkMalformed(raw, ModulationErrorCode::InvalidInput, 0);
    }
    {
        std::vector<std::byte> raw =
            BuildRawContainer(pixels, 2, kMaximumFrameDimension + 1);
        checkMalformed(raw, ModulationErrorCode::InvalidInput, 0);
    }

    // Declared pixelBytes inconsistent with width x height.
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 2);
        raw[16] = std::byte{17};
        checkMalformed(raw, ModulationErrorCode::InvalidInput, 16);
    }

    // Pixel data truncation.
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 2);
        raw.pop_back();
        checkMalformed(raw, ModulationErrorCode::TruncatedInput,
            raw.size());
    }

    // Trailing bytes.
    for (std::size_t trailingBytes = 1; trailingBytes <= 2;
        trailingBytes++)
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 2);
        raw.insert(raw.end(), trailingBytes, std::byte{0x7F});
        checkMalformed(raw, ModulationErrorCode::TrailingBytes,
            kRawFrameHeaderBytes + 16);
    }

    // Output buffer size mismatch (small and large).
    {
        const std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 2);
        std::array<std::byte, 12> tooSmall{};
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(raw),
            std::span<std::byte>(tooSmall), width, height);
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
        std::array<std::byte, 20> tooLarge{};
        const auto largeStatus = DecodeRawFrame(
            std::span<const std::byte>(raw),
            std::span<std::byte>(tooLarge), width, height);
        CHECK_FALSE(largeStatus);
        CHECK(largeStatus.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }

    // The decoder never mutates the output buffer on failure.
    {
        std::vector<std::byte> raw = BuildRawContainer(pixels, 2, 2);
        raw[0] = std::byte{0x00};
        std::array<std::byte, 16> sentinel{};
        std::fill(sentinel.begin(), sentinel.end(), std::byte{0x5A});
        const auto status = DecodeRawFrame(
            std::span<const std::byte>(raw),
            std::span<std::byte>(sentinel), width, height);
        CHECK_FALSE(status);
        for (const auto value : sentinel)
        {
            CHECK(value == std::byte{0x5A});
        }
    }
}

TEST_CASE("PBRW encode validates dimensions and input size",
    "[pbmodulation][raw][encode-validation]")
{
    const std::array<std::byte, 4> pixels{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    CHECK_FALSE(EncodeRawFrame(pixels, 0, 1));
    CHECK_FALSE(EncodeRawFrame(pixels, 1, 0));
    CHECK_FALSE(EncodeRawFrame(pixels, kMaximumFrameDimension + 1, 1));
    CHECK_FALSE(EncodeRawFrame(pixels, 1, kMaximumFrameDimension + 1));

    // 16384 x 16384 is accepted by the dimension bound but needs 1 GiB of
    // pixels: a 4-byte input must be rejected as InvalidInput.
    {
        const auto result = EncodeRawFrame(pixels, 16384, 16384);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }

    // Pixel buffer larger than the declared grid.
    {
        std::array<std::byte, 8> tooMany{};
        const auto result = EncodeRawFrame(tooMany, 1, 1);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }
}
