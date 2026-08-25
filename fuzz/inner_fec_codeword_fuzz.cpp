#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kCodewordBytes = pbinnerfec::kDvbS2ShortFrameCodewordByteCount;
constexpr std::size_t kMaximumInputBytes = kCodewordBytes + 1;
constexpr std::int16_t kPerfectLlrMagnitude = 100;

[[noreturn]] void FailInvariant()
{
    std::abort();
}

void ValidateCleanCodeword(const pbinnerfec::InnerFecProfileId profileId,
    const pbinnerfec::InnerFecProfile& profile,
    const std::span<const std::byte> codeword)
{
    std::vector<std::int16_t> llr(profile.nBits);
    for (std::size_t byteIndex = 0; byteIndex < codeword.size(); byteIndex++)
    {
        const std::uint8_t bits = std::to_integer<std::uint8_t>(codeword[byteIndex]);
        for (std::size_t bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            llr[byteIndex * 8u + bitIndex] = ((bits >> bitIndex) & 1u) != 0u
                ? -kPerfectLlrMagnitude : kPerfectLlrMagnitude;
        }
    }
    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(profileId);
    if (!decoderResult)
    {
        FailInvariant();
    }
    pbinnerfec::InnerFecDecodeOptions options;
    options.maxIterations = 16;
    options.syndromeCheckInterval = 1;
    options.offset = 2048;
    options.scaleNum = 1;
    options.scaleDen = 1;
    std::vector<std::byte> decoded(codeword.size(), std::byte{0xA5});
    const auto decodeResult = decoderResult.Value().Decode(llr, options, decoded);
    if (!decodeResult || !std::equal(decoded.begin(), decoded.end(), codeword.begin()))
    {
        FailInvariant();
    }
    std::vector<std::byte> reencoded(codeword.size(), std::byte{0x5A});
    if (!pbinnerfec::EncodeQcLdpcCodeword(profileId,
        std::span<const std::byte>(decoded).first(profile.GetInfoByteCount()), reencoded) ||
        reencoded != decoded)
    {
        FailInvariant();
    }
    if (profileId == pbinnerfec::kInnerFecProfileIdRobust)
    {
        const auto extracted = pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(decoded).first(profile.GetInfoByteCount()));
        if (extracted && !pbprotocol::ParseTransportBlock(extracted.Value()))
        {
            FailInvariant();
        }
    }
}

void FuzzOne(const std::span<const std::byte> input)
{
    if (input.size() > kMaximumInputBytes)
    {
        return;
    }
    constexpr std::array<pbinnerfec::InnerFecProfileId, 3> profileIds{
        pbinnerfec::kInnerFecProfileIdRobust,
        pbinnerfec::kInnerFecProfileIdBalanced,
        pbinnerfec::kInnerFecProfileIdFast};
    for (const pbinnerfec::InnerFecProfileId profileId : profileIds)
    {
        const auto syndrome = pbinnerfec::ComputeQcLdpcSyndrome(profileId, input);
        if (input.size() != kCodewordBytes)
        {
            if (syndrome)
            {
                FailInvariant();
            }
            continue;
        }
        if (!syndrome)
        {
            FailInvariant();
        }
        if (syndrome.Value())
        {
            const auto* profile = pbinnerfec::GetInnerFecProfile(profileId);
            if (profile == nullptr)
            {
                FailInvariant();
            }
            ValidateCleanCodeword(profileId, *profile, input);
        }
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

[[nodiscard]] std::vector<std::byte> MakeRobustCodeword(const std::uint64_t seed)
{
    const auto* profile = pbinnerfec::GetInnerFecProfile(pbinnerfec::kInnerFecProfileIdRobust);
    if (profile == nullptr)
    {
        FailInvariant();
    }
    SplitMix64 rng(seed);
    std::vector<std::byte> info(profile->GetInfoByteCount());
    for (std::byte& value : info)
    {
        value = std::byte{static_cast<std::uint8_t>(rng.Next())};
    }
    std::vector<std::byte> codeword(profile->GetCodewordByteCount());
    if (!pbinnerfec::EncodeQcLdpcCodeword(
        pbinnerfec::kInnerFecProfileIdRobust, info, codeword))
    {
        FailInvariant();
    }
    return codeword;
}

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
        ? std::stoull(arguments[2]) : 0xFEC0DEC0FFEE1234ULL;
    SplitMix64 rng(seed);
    const auto canonical = MakeRobustCodeword(seed);
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        auto bytes = canonical;
        const std::size_t mutationCount = static_cast<std::size_t>(rng.Next() % 9u);
        for (std::size_t mutation = 0; mutation < mutationCount; mutation++)
        {
            const std::size_t offset = static_cast<std::size_t>(rng.Next() % bytes.size());
            bytes[offset] ^= std::byte{static_cast<std::uint8_t>(1u << (rng.Next() % 8u))};
        }
        FuzzOne(bytes);
    }
    std::cout << "FUZZ_COMPLETED iterations=" << iterations << " seed=" << seed << "\n";
    return 0;
}
#endif
