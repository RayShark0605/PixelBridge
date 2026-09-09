#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pbapp
{

struct RecordedPixelFrame
{
    std::span<const std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    std::int64_t pts = 0;
    std::int32_t timeBaseNumerator = 0;
    std::int32_t timeBaseDenominator = 0;
    std::int64_t duration = 0;
};

enum class RecordedPixelRead : std::uint8_t
{
    Frame,
    EndOfFile,
    Error
};

// Dedicated offline tool input, not a DecoderConfig/GUI payload option.
// A returned frame is borrowed only until the next ReadNext. No source payload,
// expected codeword, descriptor, sender truth or admission callbacks exist here.
class RecordedPixelSource
{
public:
    virtual ~RecordedPixelSource() = default;
    [[nodiscard]] virtual RecordedPixelRead ReadNext(RecordedPixelFrame& output, std::string& error) = 0;
};

} // namespace pbapp
