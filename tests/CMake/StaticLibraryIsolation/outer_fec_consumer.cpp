#include "pbouterfec/wirehair_v2.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

int main()
{
    std::array<std::byte, 17> message{};
    for (std::size_t byteIndex = 0; byteIndex < message.size(); byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>(byteIndex + 1U));
    }

    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(message, 16);
    if (!encoderResult)
    {
        return 1;
    }
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    if (encoder.GetBlockCount() != 2)
    {
        return 2;
    }

    std::array<std::byte, 16> firstBlock{};
    const auto encodeResult = encoder.EncodeBlock(0, firstBlock);
    if (!encodeResult || encodeResult.Value() != firstBlock.size())
    {
        return 3;
    }
    if (!std::equal(
            firstBlock.begin(),
            firstBlock.end(),
            message.begin(),
            message.begin() + static_cast<std::ptrdiff_t>(firstBlock.size())))
    {
        return 4;
    }
    return 0;
}
