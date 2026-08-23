#include "pbouterfec/wirehair_v2.h"

#include <array>
#include <cstddef>
#include <utility>

int main()
{
    const std::array<std::byte, 17> message{};
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

    return 0;
}
