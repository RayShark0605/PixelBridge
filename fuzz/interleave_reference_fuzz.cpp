#include "pbinterleave/interleave_reference.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumInputBytes = pbinterleave::kInterleaveRegionBytes + 1;

[[noreturn]] void FailInvariant()
{
    std::abort();
}

[[nodiscard]] std::uint64_t DeriveFrameSequence(
    const std::span<const std::byte> input) noexcept
{
    std::uint64_t value = 0;
    const std::size_t count = std::min<std::size_t>(input.size(), 8);
    for (std::size_t index = 0; index < count; index++)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[index])) <<
            static_cast<unsigned int>(index * 8u);
    }
    return value;
}

void FuzzOne(const std::span<const std::byte> input)
{
    if (input.size() > kMaximumInputBytes)
    {
        return;
    }
    const std::uint64_t frameSequence = DeriveFrameSequence(input);
    if (input.size() != pbinterleave::kInterleaveRegionBytes)
    {
        std::vector<std::byte> output(pbinterleave::kInterleaveRegionBytes, std::byte{0xA5});
        const auto before = output;
        const auto applyStatus = pbinterleave::ApplyInterleave(input, output, frameSequence);
        const auto reverseStatus = pbinterleave::ReverseInterleave(input, output, frameSequence);
        if (applyStatus || reverseStatus || output != before)
        {
            FailInvariant();
        }
        return;
    }

    std::vector<std::byte> physical(pbinterleave::kInterleaveRegionBytes, std::byte{0xA5});
    if (!pbinterleave::ApplyInterleave(input, physical, frameSequence))
    {
        FailInvariant();
    }
    std::vector<std::byte> restored(pbinterleave::kInterleaveRegionBytes, std::byte{0x5A});
    if (!pbinterleave::ReverseInterleave(physical, restored, frameSequence) ||
        !std::equal(restored.begin(), restored.end(), input.begin()))
    {
        FailInvariant();
    }
    std::vector<std::byte> alias(input.begin(), input.end());
    const auto beforeAlias = alias;
    const auto aliasSpan = std::span<std::byte>(alias);
    const auto aliasStatus = pbinterleave::ApplyInterleave(
        std::span<const std::byte>(aliasSpan), aliasSpan, frameSequence);
    if (aliasStatus || aliasStatus.Error().code != pbinterleave::InterleaveErrorCode::OverlappingSpans ||
        alias != beforeAlias)
    {
        FailInvariant();
    }
}

class SplitMix64
{
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] bool ReadFile(const std::string& path, std::vector<std::byte>& bytes)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumInputBytes)
    {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(stream);
}

} // namespace

#if defined(PB_USE_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    FuzzOne(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
    return 0;
}
#else
int main(const int argumentCount, char* arguments[])
{
    if (argumentCount == 3 && std::string(arguments[1]) == "--input")
    {
        std::vector<std::byte> bytes;
        if (!ReadFile(arguments[2], bytes))
        {
            return 2;
        }
        FuzzOne(bytes);
        std::cout << "CORPUS_REPLAY_NO_CRASH\n";
        return 0;
    }
    const std::uint64_t iterations = argumentCount >= 2
        ? std::stoull(arguments[1]) : 1000;
    const std::uint64_t seed = argumentCount >= 3
        ? std::stoull(arguments[2]) : 0x1A2B3C4D5E6F7081ULL;
    SplitMix64 rng(seed);
    std::vector<std::byte> bytes(pbinterleave::kInterleaveRegionBytes);
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        for (std::size_t index = 0; index < bytes.size(); index++)
        {
            bytes[index] = std::byte{static_cast<std::uint8_t>(rng.Next())};
        }
        if ((rng.Next() & 31u) == 0u)
        {
            bytes.resize(pbinterleave::kInterleaveRegionBytes - 1);
            FuzzOne(bytes);
            bytes.resize(pbinterleave::kInterleaveRegionBytes);
        }
        FuzzOne(bytes);
    }
    std::cout << "FUZZ_COMPLETED iterations=" << iterations << " seed=" << seed << "\n";
    return 0;
}
#endif
