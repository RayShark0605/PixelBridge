#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <array>
#include <cmath>

namespace pbmodulation::detail
{
using Erasure = LocalDesktopErasureReason;

inline std::size_t BytesPerPixel(const LumaPixelFormat format) noexcept
{
    switch (format)
    {
    case LumaPixelFormat::Gray8: return 1;
    case LumaPixelFormat::Bgra8:
    case LumaPixelFormat::R10G10B10A2: return 4;
    case LumaPixelFormat::Fp16LinearSdr: return 8;
    default: return 0;
    }
}

inline std::uint16_t Read16(const std::byte* bytes) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[0]) |
                                     (std::to_integer<std::uint16_t>(bytes[1]) << 8));
}

inline std::uint32_t Read32(const std::byte* bytes) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[0]) | (std::to_integer<std::uint32_t>(bytes[1]) << 8) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16) | (std::to_integer<std::uint32_t>(bytes[3]) << 24);
}

inline bool DecodeHalf(const std::uint16_t bits, double& value) noexcept
{
    const auto exponent = static_cast<unsigned>((bits >> 10) & 31u);
    const auto fraction = static_cast<unsigned>(bits & 1023u);
    if (exponent == 31)
    {
        return false;
    }
    const double magnitude = exponent == 0 ? std::ldexp(static_cast<double>(fraction), -24) :
                                             std::ldexp(static_cast<double>(1024u + fraction), static_cast<int>(exponent) - 25);
    value = (bits & 0x8000u) == 0 ? magnitude : -magnitude;
    return true;
}

// Constructed only after one complete stride/footprint validation. Every read
// still checks its coordinates and consumes a bounded work token. No image-
// sized scratch, allocation, normalized copy, or mutable process state exists.
class LumaReader
{
public:
    LumaReader(const LumaView& view, const std::uint64_t maximumWork, const bool exactIntegerLuma = false) noexcept :
        view_(view), pixelBytes_(BytesPerPixel(view.pixelFormat)), maximumWork_(maximumWork), exactIntegerLuma_(exactIntegerLuma)
    {
    }

    [[nodiscard]] bool Charge(const std::uint64_t units = 1) noexcept
    {
        if (error_ != Erasure::None)
        {
            return false;
        }
        if (units > maximumWork_ - workUnits_)
        {
            error_ = Erasure::WorkBudgetExceeded;
            return false;
        }
        workUnits_ += units;
        return true;
    }

    [[nodiscard]] bool Contains(const double x, const double y) const noexcept
    {
        return std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 &&
               x <= static_cast<double>(view_.width - 1u) && y <= static_cast<double>(view_.height - 1u);
    }

    [[nodiscard]] bool Pixel(const std::uint32_t x, const std::uint32_t y, double& output, bool* const clipped = nullptr) noexcept
    {
        if (x >= view_.width || y >= view_.height)
        {
            error_ = Erasure::SampleOutOfBounds;
            return false;
        }
        if (!Charge())
        {
            return false;
        }
        const auto* bytes = view_.pixels.data() + static_cast<std::size_t>(y) * view_.rowPitch + static_cast<std::size_t>(x) * pixelBytes_;
        double value = 0;
        bool sourceClipped = false;
        switch (view_.pixelFormat)
        {
        case LumaPixelFormat::Gray8:
            value = std::to_integer<std::uint8_t>(bytes[0]);
            sourceClipped = value == 0 || value == 255;
            break;
        case LumaPixelFormat::Bgra8:
            sourceClipped = bytes[0] == std::byte{0} || bytes[0] == std::byte{255} || bytes[1] == std::byte{0} ||
                bytes[1] == std::byte{255} || bytes[2] == std::byte{0} || bytes[2] == std::byte{255};
            // DesktopLevels uses exact rational integer-code luma before the
            // final division. Neutral BGRA bytes then remain exact integers:
            // a strict variance/midpoint boundary is not perturbed by three
            // rounded floating multiplies. The old locator/sample API retains
            // its original arithmetic, without re-pinning any existing vector.
            value = exactIntegerLuma_ ? (722u * std::to_integer<unsigned>(bytes[0]) + 7152u * std::to_integer<unsigned>(bytes[1]) +
                    2126u * std::to_integer<unsigned>(bytes[2])) / 10000.0 :
                    0.0722 * std::to_integer<std::uint8_t>(bytes[0]) + 0.7152 * std::to_integer<std::uint8_t>(bytes[1]) + 0.2126 * std::to_integer<std::uint8_t>(bytes[2]);
            break;
        case LumaPixelFormat::R10G10B10A2:
        {
            const auto packed = Read32(bytes);
            sourceClipped = (packed & 1023u) == 0 || (packed & 1023u) == 1023 || ((packed >> 10) & 1023u) == 0 ||
                ((packed >> 10) & 1023u) == 1023 || ((packed >> 20) & 1023u) == 0 || ((packed >> 20) & 1023u) == 1023;
            value = exactIntegerLuma_ ? (2126u * (packed & 1023u) + 7152u * ((packed >> 10) & 1023u) + 722u * ((packed >> 20) & 1023u)) *
                     255.0 / 10230000.0 : (0.2126 * (packed & 1023u) + 0.7152 * ((packed >> 10) & 1023u) + 0.0722 * ((packed >> 20) & 1023u)) * (255.0 / 1023.0);
            break;
        }
        case LumaPixelFormat::Fp16LinearSdr:
        {
            std::array<double, 4> components{};
            for (std::size_t component = 0; component < components.size(); component++)
            {
                if (!DecodeHalf(Read16(bytes + component * 2), components[component]))
                {
                    error_ = Erasure::NonFinitePixel;
                    return false;
                }
                if (components[component] < 0 || components[component] > 1)
                {
                    error_ = Erasure::InvalidPixelValue;
                    return false;
                }
                // Alpha is not a transmitted Luma component; opaque alpha=1
                // must not make every valid SDR pixel look clipped.
                sourceClipped = sourceClipped || (component < 3 && (components[component] == 0 || components[component] == 1));
            }
            const double linear = 0.2126 * components[0] + 0.7152 * components[1] + 0.0722 * components[2];
            value = exactIntegerLuma_ && linear == 1.0 ? 255.0 :
                255.0 * (linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055);
            break;
        }
        default:
            error_ = Erasure::UnsupportedFormat;
            return false;
        }
        output = value;
        if (clipped != nullptr)
        {
            *clipped = sourceClipped || value <= 0 || value >= 255;
        }
        return true;
    }

    [[nodiscard]] bool Sample(const double x, const double y, double& output) noexcept
    {
        if (!Contains(x, y))
        {
            error_ = std::isfinite(x) && std::isfinite(y) ? Erasure::SampleOutOfBounds : Erasure::NonFinitePixel;
            return false;
        }
        const auto left = static_cast<std::uint32_t>(std::floor(x));
        const auto top = static_cast<std::uint32_t>(std::floor(y));
        const double horizontal = x - left;
        const double vertical = y - top;
        const auto right = horizontal == 0 ? left : left + 1u;
        const auto bottom = vertical == 0 ? top : top + 1u;
        double topLeft = 0;
        double topRight = 0;
        double bottomLeft = 0;
        double bottomRight = 0;
        if (!Pixel(left, top, topLeft))
        {
            return false;
        }
        topRight = topLeft;
        if (right != left && !Pixel(right, top, topRight))
        {
            return false;
        }
        bottomLeft = topLeft;
        bottomRight = topRight;
        if (bottom != top && (!Pixel(left, bottom, bottomLeft) || (right != left && !Pixel(right, bottom, bottomRight))))
        {
            return false;
        }
        if (right == left)
        {
            bottomRight = bottomLeft;
        }
        const double upper = topLeft + horizontal * (topRight - topLeft);
        const double lower = bottomLeft + horizontal * (bottomRight - bottomLeft);
        output = upper + vertical * (lower - upper);
        return true;
    }

    [[nodiscard]] Erasure Error() const noexcept
    {
        return error_;
    }

    [[nodiscard]] std::uint64_t WorkUnits() const noexcept
    {
        return workUnits_;
    }

    [[nodiscard]] std::uint32_t Width() const noexcept
    {
        return view_.width;
    }

    [[nodiscard]] std::uint32_t Height() const noexcept
    {
        return view_.height;
    }

private:
    const LumaView& view_;
    const std::size_t pixelBytes_;
    const std::uint64_t maximumWork_;
    const bool exactIntegerLuma_;
    std::uint64_t workUnits_ = 0;
    Erasure error_ = Erasure::None;
};

} // namespace pbmodulation::detail
