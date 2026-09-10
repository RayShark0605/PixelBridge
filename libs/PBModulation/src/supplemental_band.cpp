#include "pbmodulation/supplemental_band.h"

#include "band_rs.h"

#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation
{
namespace
{

using BandMessage = std::array<std::byte, kSupplementalBandMessageBytes>;
using BandCodeword = std::array<std::byte, kSupplementalBandCodewordBytes>;
constexpr std::array<std::byte, 4> kBandMagic{std::byte{'P'}, std::byte{'B'}, std::byte{'B'}, std::byte{'1'}};
constexpr std::size_t kBandLumaHigh = 224;
constexpr std::size_t kBandLumaLow = 32;

// Cell grid of the rendered codeword: 148 columns x 14 rows of 4x4 cells
// offset by (8, 4) inside the 608x64 patch.
constexpr std::size_t kBandCellColumns = 148;
constexpr std::size_t kBandCellOffsetX = 8;
constexpr std::size_t kBandCellOffsetY = 4;
constexpr std::size_t kBandCellStride = 4;

void StoreLittle(const std::span<std::byte> bytes, const std::size_t offset, const std::size_t count, const std::uint64_t value) noexcept
{
    for (std::size_t index = 0; index < count; index++)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
    }
}

std::uint64_t LoadLittle(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t count) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; index++)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8);
    }
    return value;
}

void SealMessage(BandMessage& message) noexcept
{
    StoreLittle(message, 24, 4, 0);
    StoreLittle(message, 24, 4, pbprotocol::ComputeCrc32c(message));
}

bool PackMessage(const std::span<const std::byte> record, const pbprotocol::SessionTag sessionTag,
    const std::uint64_t frameSequence, BandMessage& output) noexcept
{
    if (record.size() > kSupplementalBandMaximumRecordBytes)
    {
        return false;
    }
    const auto parsed = pbprotocol::ParseControlRecord(record);
    if (!parsed || parsed.Value().sessionTag != sessionTag)
    {
        return false;
    }
    output = BandMessage{};
    std::copy(kBandMagic.begin(), kBandMagic.end(), output.begin());
    output[4] = std::byte{1};
    StoreLittle(output, 6, 2, record.size());
    StoreLittle(output, 8, 8, sessionTag.value);
    StoreLittle(output, 16, 8, frameSequence);
    std::copy(record.begin(), record.end(), output.begin() + kSupplementalBandHeaderBytes);
    SealMessage(output);
    return true;
}

void RenderCodeword(const BandCodeword& codeword, const std::span<std::byte> patch) noexcept
{
    for (std::size_t pixel = 0; pixel < patch.size(); pixel += 4)
    {
        patch[pixel] = patch[pixel + 1] = patch[pixel + 2] = std::byte{128};
        patch[pixel + 3] = std::byte{255};
    }
    for (std::size_t bit = 0; bit < codeword.size() * 8; bit++)
    {
        const std::size_t cellX = kBandCellOffsetX + (bit % kBandCellColumns) * kBandCellStride;
        const std::size_t cellY = kBandCellOffsetY + (bit / kBandCellColumns) * kBandCellStride;
        const std::byte level = (std::to_integer<unsigned>(codeword[bit / 8]) & (1U << (bit % 8))) != 0 ?
            static_cast<std::byte>(kBandLumaHigh) : static_cast<std::byte>(kBandLumaLow);
        for (std::size_t row = 0; row < 4; row++)
        {
            for (std::size_t column = 0; column < 4; column++)
            {
                const std::size_t pixel = ((cellY + row) * 608 + cellX + column) * 4;
                patch[pixel] = patch[pixel + 1] = patch[pixel + 2] = level;
            }
        }
    }
}

[[nodiscard]] bool SamplePixel(const LumaView& view, const LocalDesktopGeometry& geometry,
    const std::uint32_t logicalX, const std::uint32_t logicalY, std::byte* const output) noexcept
{
    const double physicalX = geometry.originX + geometry.scaleX * (static_cast<double>(logicalX) + 0.5) - 0.5;
    const double physicalY = geometry.originY + geometry.scaleY * (static_cast<double>(logicalY) + 0.5) - 0.5;
    if (!std::isfinite(physicalX) || !std::isfinite(physicalY) || physicalX < 0 || physicalY < 0 ||
        physicalX > static_cast<double>(view.width - 1) || physicalY > static_cast<double>(view.height - 1))
    {
        return false;
    }
    // A provider downscale has already integrated source support into each
    // captured pixel; bilinear interpolation on that axis would apply a second
    // low-pass filter. Nearest sample downscaled axes, keep continuous
    // geometry on magnified axes. Channel rounding is round-half-up; at exact
    // 1:1 every interpolated value is an exact integer.
    const bool horizontalDownscale = geometry.scaleX < 1;
    const bool verticalDownscale = geometry.scaleY < 1;
    const std::uint32_t left = horizontalDownscale ?
        std::min(static_cast<std::uint32_t>(std::floor(physicalX + 0.5)), view.width - 1) :
        static_cast<std::uint32_t>(std::floor(physicalX));
    const std::uint32_t top = verticalDownscale ?
        std::min(static_cast<std::uint32_t>(std::floor(physicalY + 0.5)), view.height - 1) :
        static_cast<std::uint32_t>(std::floor(physicalY));
    const double horizontal = horizontalDownscale ? 0 : physicalX - left;
    const double vertical = verticalDownscale ? 0 : physicalY - top;
    const std::uint32_t right = horizontal == 0 ? left : left + 1;
    const std::uint32_t bottom = vertical == 0 ? top : top + 1;
    if (right >= view.width || bottom >= view.height)
    {
        return false;
    }
    const auto Read = [&](const std::uint32_t x, const std::uint32_t y)
    {
        const std::byte* const pixel = view.pixels.data() + static_cast<std::size_t>(y) * view.rowPitch +
            static_cast<std::size_t>(x) * 4;
        return std::array<double, 3>{static_cast<double>(std::to_integer<std::uint8_t>(pixel[0])),
            static_cast<double>(std::to_integer<std::uint8_t>(pixel[1])),
            static_cast<double>(std::to_integer<std::uint8_t>(pixel[2]))};
    };
    const std::array<double, 3> topLeft = Read(left, top);
    const std::array<double, 3> topRight = Read(right, top);
    const std::array<double, 3> bottomLeft = Read(left, bottom);
    const std::array<double, 3> bottomRight = Read(right, bottom);
    for (std::size_t channel = 0; channel < 3; channel++)
    {
        const double upper = topLeft[channel] + horizontal * (topRight[channel] - topLeft[channel]);
        const double lower = bottomLeft[channel] + horizontal * (bottomRight[channel] - bottomLeft[channel]);
        const double value = upper + vertical * (lower - upper);
        output[channel] = static_cast<std::byte>(static_cast<unsigned>(std::floor(value + 0.5)) & 255U);
    }
    output[3] = std::byte{255};
    return true;
}

} // namespace

bool RenderSupplementalBand(const std::span<const std::byte> controlRecord, const pbprotocol::SessionTag sessionTag,
    const std::uint64_t frameSequence, const std::span<std::byte> patch)
{
    if (patch.size() != kSupplementalBandPatchBytes)
    {
        return false;
    }
    BandMessage message{};
    if (!PackMessage(controlRecord, sessionTag, frameSequence, message))
    {
        return false;
    }
    BandCodeword codeword{};
    if (!band_detail::EncodeBandRs(message, codeword))
    {
        return false;
    }
    RenderCodeword(codeword, patch);
    return true;
}

bool BlitSupplementalBand(const std::span<const std::byte> patch, const std::span<std::byte> frameBgra, const std::size_t bandIndex)
{
    if (patch.size() != kSupplementalBandPatchBytes || bandIndex >= kSupplementalBands.size() ||
        frameBgra.size() != 1920 * 1080 * 4)
    {
        return false;
    }
    const auto& region = kSupplementalBands[bandIndex];
    for (std::size_t row = 0; row < region.height; row++)
    {
        std::copy_n(patch.data() + row * 608 * 4, 608 * 4,
            frameBgra.data() + ((region.y + row) * 1920 + region.x) * 4);
    }
    return true;
}

bool SampleSupplementalBand(const LumaView& view, const LocalDesktopGeometry& geometry,
    const std::size_t bandIndex, const std::span<std::byte> patch)
{
    if (patch.size() != kSupplementalBandPatchBytes || bandIndex >= kSupplementalBands.size() ||
        view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        return false;
    }
    const auto& region = kSupplementalBands[bandIndex];
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            if (!SamplePixel(view, geometry, region.x + column, region.y + row,
                patch.data() + (static_cast<std::size_t>(row) * 608 + column) * 4))
            {
                return false;
            }
        }
    }
    return true;
}

SupplementalBandDecodeOutcome DecodeSupplementalBand(const LumaView& view, const LocalDesktopGeometry& geometry,
    const std::size_t bandIndex, const pbprotocol::SessionTag expectedSessionTag, const std::uint64_t expectedFrameSequence,
    const std::span<std::byte> patchScratch, const std::span<std::byte> recordOutput)
{
    SupplementalBandDecodeOutcome outcome{};
    if (patchScratch.size() != kSupplementalBandPatchBytes ||
        recordOutput.size() < kSupplementalBandMaximumRecordBytes ||
        !SampleSupplementalBand(view, geometry, bandIndex, patchScratch))
    {
        outcome.status = SupplementalBandDecodeStatus::SamplingRejected;
        return outcome;
    }
    BandCodeword word{};
    for (std::size_t bit = 0; bit < word.size() * 8; bit++)
    {
        const std::size_t cellX = kBandCellOffsetX + (bit % kBandCellColumns) * kBandCellStride;
        const std::size_t cellY = kBandCellOffsetY + (bit / kBandCellColumns) * kBandCellStride;
        unsigned total = 0;
        for (std::size_t row = 1; row <= 2; row++)
        {
            for (std::size_t column = 1; column <= 2; column++)
            {
                const std::size_t pixel = ((cellY + row) * 608 + cellX + column) * 4;
                total += (29U * std::to_integer<unsigned>(patchScratch[pixel]) +
                    150U * std::to_integer<unsigned>(patchScratch[pixel + 1]) +
                    77U * std::to_integer<unsigned>(patchScratch[pixel + 2])) >> 8;
            }
        }
        const unsigned luma = total / 4;
        if (luma > 96 && luma < 160)
        {
            outcome.status = SupplementalBandDecodeStatus::AmbiguousCell;
            return outcome;
        }
        if (luma >= 128)
        {
            word[bit / 8] |= static_cast<std::byte>(1U << (bit % 8));
        }
    }
    BandMessage message{};
    const auto corrected = band_detail::DecodeBandRs(word, message);
    if (!corrected)
    {
        outcome.status = SupplementalBandDecodeStatus::RsFailure;
        return outcome;
    }
    outcome.correctedSymbols = corrected.correctedSymbols;
    if (!std::equal(kBandMagic.begin(), kBandMagic.end(), message.begin()) || message[4] != std::byte{1} || message[5] != std::byte{0})
    {
        outcome.status = SupplementalBandDecodeStatus::IdentityRejected;
        return outcome;
    }
    const std::size_t length = static_cast<std::size_t>(LoadLittle(message, 6, 2));
    if (length < pbprotocol::kMinimumControlRecordBytes || length > kSupplementalBandMaximumRecordBytes ||
        LoadLittle(message, 8, 8) != expectedSessionTag.value || LoadLittle(message, 16, 8) != expectedFrameSequence)
    {
        outcome.status = SupplementalBandDecodeStatus::IdentityRejected;
        return outcome;
    }
    const auto crc = LoadLittle(message, 24, 4);
    StoreLittle(message, 24, 4, 0);
    if (pbprotocol::ComputeCrc32c(message) != crc)
    {
        outcome.status = SupplementalBandDecodeStatus::CrcRejected;
        return outcome;
    }
    if (!std::all_of(message.begin() + kSupplementalBandHeaderBytes + length, message.end(),
        [](const auto value) { return value == std::byte{0}; }))
    {
        outcome.status = SupplementalBandDecodeStatus::RecordRejected;
        return outcome;
    }
    const auto record = std::span<const std::byte>(message).subspan(kSupplementalBandHeaderBytes, length);
    const auto parsed = pbprotocol::ParseControlRecord(record);
    if (!parsed || parsed.Value().sessionTag != expectedSessionTag)
    {
        outcome.status = SupplementalBandDecodeStatus::RecordRejected;
        return outcome;
    }
    StoreLittle(message, 24, 4, crc);
    std::copy(record.begin(), record.end(), recordOutput.begin());
    outcome.status = SupplementalBandDecodeStatus::Admitted;
    outcome.recordBytes = length;
    return outcome;
}

} // namespace pbmodulation
