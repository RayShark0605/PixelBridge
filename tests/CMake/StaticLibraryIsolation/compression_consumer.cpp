#include "pbcompression/segment_compression.h"
#include "pbcompression/segment_decompression.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

int main()
{
    constexpr std::size_t rawBytes = 64 * 1024;
    std::vector<std::byte> input(rawBytes);
    for (std::size_t byteIndex = 0; byteIndex < input.size(); byteIndex++)
    {
        input[byteIndex] = static_cast<std::byte>(
            0x41U + static_cast<std::uint8_t>(byteIndex % 26U));
    }

    const pbcompression::CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    if (!encodedResult)
    {
        return 1;
    }

    const pbcompression::EncodedSegment& encodedSegment =
        encodedResult.Value();
    const pbcompression::DecompressionLimits limits;
    const auto decodedResult = pbcompression::DecompressSegment(
        encodedSegment.codec,
        std::span<const std::byte>(encodedSegment.bytes),
        encodedSegment.bytes.size(),
        input.size(),
        limits);
    if (!decodedResult)
    {
        return 2;
    }
    if (decodedResult.Value().size() != input.size())
    {
        return 3;
    }
    if (!std::equal(
            decodedResult.Value().begin(),
            decodedResult.Value().end(),
            input.begin()))
    {
        return 4;
    }
    return 0;
}
