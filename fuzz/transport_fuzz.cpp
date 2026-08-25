#include "pbprotocol/crc32c.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumInputBytes = pbprotocol::kTransportMaximumBlockBytes;

[[noreturn]] void FailInvariant()
{
    std::abort();
}

[[nodiscard]] bool BytesEqual(const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

void StoreU16(const std::span<std::byte> bytes, const std::size_t offset,
    const std::uint16_t value) noexcept
{
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value & 0xFFu)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void StoreU32(const std::span<std::byte> bytes, const std::size_t offset,
    const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8u))};
    }
}

void RefreshHeaderCrc(std::vector<std::byte>& bytes)
{
    const std::uint32_t crc = static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(pbprotocol::kTransportHeaderCrcCoverageBytes)));
    StoreU32(bytes, pbprotocol::kTransportHeaderCrcOffset, crc);
}

[[nodiscard]] std::vector<std::byte> MakeCanonicalBlock()
{
    pbprotocol::TransportBlockHeader header;
    header.blockType = pbprotocol::kTransportBlockTypeData;
    header.sessionTag.value = 0x81DF204BD997BAD0ULL;
    header.payloadBytes = 8;
    const std::array<std::byte, 8> payload{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80}};
    std::vector<std::byte> bytes(pbprotocol::kTransportMinimumBlockBytes + payload.size());
    if (!pbprotocol::SerializeTransportBlock(header, payload, bytes))
    {
        FailInvariant();
    }
    return bytes;
}

void FuzzOne(const std::span<const std::byte> input)
{
    if (input.size() > kMaximumInputBytes)
    {
        return;
    }
    const auto parsed = pbprotocol::ParseTransportBlock(input);
    if (parsed)
    {
        std::vector<std::byte> serialized(input.size(), std::byte{0xA5});
        if (!pbprotocol::SerializeTransportBlock(
            parsed.Value().header, parsed.Value().payload, serialized) ||
            !BytesEqual(serialized, input))
        {
            FailInvariant();
        }
        std::vector<std::byte> framed(input.size(), std::byte{0x5A});
        if (!pbprotocol::FrameTransportBlockIntoInfoBlock(input, input.size(), framed) ||
            !BytesEqual(framed, input))
        {
            FailInvariant();
        }
    }

    std::vector<std::byte> output(input.size(), std::byte{0xA5});
    const std::vector<std::byte> before = output;
    const auto framed = pbprotocol::FrameTransportBlockIntoInfoBlock(
        input, input.size(), output);
    if (!framed && output != before)
    {
        FailInvariant();
    }

    const auto extracted = pbprotocol::ExtractTransportBlockFromInfoBlock(input);
    if (extracted)
    {
        const auto extractedParsed = pbprotocol::ParseTransportBlock(extracted.Value());
        if (!extractedParsed)
        {
            FailInvariant();
        }
        std::vector<std::byte> reframed(input.size(), std::byte{0xA5});
        if (!pbprotocol::FrameTransportBlockIntoInfoBlock(
            extracted.Value(), input.size(), reframed) || !BytesEqual(reframed, input))
        {
            FailInvariant();
        }
    }
}

[[nodiscard]] int RunStructuredSelfTest()
{
    const auto canonical = MakeCanonicalBlock();
    if (!pbprotocol::ParseTransportBlock(canonical))
    {
        return 1;
    }
    struct SemanticCase
    {
        std::size_t offset = 0;
        std::byte value{};
        pbprotocol::ProtocolErrorCode expected = pbprotocol::ProtocolErrorCode::None;
    };
    const std::array<SemanticCase, 4> cases{
        SemanticCase{0, std::byte{2}, pbprotocol::ProtocolErrorCode::InvalidEnumValue},
        SemanticCase{1, std::byte{1}, pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor},
        SemanticCase{2, std::byte{1}, pbprotocol::ProtocolErrorCode::NonZeroReservedBits},
        SemanticCase{26, std::byte{1}, pbprotocol::ProtocolErrorCode::NonZeroReservedBits}};
    for (const SemanticCase& semanticCase : cases)
    {
        auto mutated = canonical;
        mutated[semanticCase.offset] = semanticCase.value;
        RefreshHeaderCrc(mutated);
        const auto result = pbprotocol::ParseTransportBlock(mutated);
        if (result || result.Error().code != semanticCase.expected)
        {
            const auto actual = result ? pbprotocol::ProtocolErrorCode::None : result.Error().code;
            std::cerr << "structured_case_offset=" << semanticCase.offset
                      << " expected=" << static_cast<unsigned>(semanticCase.expected)
                      << " actual=" << static_cast<unsigned>(actual) << "\n";
            return 1;
        }
    }
    auto headerFirst = canonical;
    StoreU16(headerFirst, pbprotocol::kTransportPayloadBytesOffset,
        std::numeric_limits<std::uint16_t>::max());
    const auto headerFirstResult = pbprotocol::ParseTransportBlock(headerFirst);
    if (headerFirstResult || headerFirstResult.Error().code != pbprotocol::ProtocolErrorCode::CrcMismatch)
    {
        const auto actual = headerFirstResult ? pbprotocol::ProtocolErrorCode::None : headerFirstResult.Error().code;
        std::cerr << "header_crc_precedence expected="
                  << static_cast<unsigned>(pbprotocol::ProtocolErrorCode::CrcMismatch)
                  << " actual=" << static_cast<unsigned>(actual) << "\n";
        return 1;
    }
    auto lengthSemantic = canonical;
    StoreU16(lengthSemantic, pbprotocol::kTransportPayloadBytesOffset, 1);
    RefreshHeaderCrc(lengthSemantic);
    const auto lengthResult = pbprotocol::ParseTransportBlock(lengthSemantic);
    if (lengthResult || lengthResult.Error().code != pbprotocol::ProtocolErrorCode::TrailingBytes)
    {
        const auto actual = lengthResult ? pbprotocol::ProtocolErrorCode::None : lengthResult.Error().code;
        std::cerr << "declared_length_semantic expected="
                  << static_cast<unsigned>(pbprotocol::ProtocolErrorCode::TrailingBytes)
                  << " actual=" << static_cast<unsigned>(actual) << "\n";
        return 1;
    }
    std::vector<std::byte> padded(1350, std::byte{0});
    std::copy(canonical.begin(), canonical.end(), padded.begin());
    if (!pbprotocol::ExtractTransportBlockFromInfoBlock(padded))
    {
        return 1;
    }
    padded.back() = std::byte{1};
    const auto dirty = pbprotocol::ExtractTransportBlockFromInfoBlock(padded);
    if (dirty || dirty.Error().code != pbprotocol::ProtocolErrorCode::NonCanonicalPadding)
    {
        return 1;
    }
    std::cout << "TRANSPORT_STRUCTURED_SELF_TEST_COMPLETED\n";
    return 0;
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
    if (argumentCount == 2 && std::string(arguments[1]) == "--self-test")
    {
        return RunStructuredSelfTest();
    }
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
        ? std::stoull(arguments[2]) : 0xC0DEC0DE12345678ULL;
    SplitMix64 rng(seed);
    const auto canonical = MakeCanonicalBlock();
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        auto bytes = canonical;
        const std::size_t mutationCount = static_cast<std::size_t>(rng.Next() % 9u);
        for (std::size_t mutation = 0; mutation < mutationCount; mutation++)
        {
            const std::size_t offset = static_cast<std::size_t>(rng.Next() % bytes.size());
            bytes[offset] ^= std::byte{static_cast<std::uint8_t>(1u << (rng.Next() % 8u))};
        }
        if ((rng.Next() & 7u) == 0u)
        {
            bytes.resize(static_cast<std::size_t>(rng.Next() % (bytes.size() + 1u)));
        }
        FuzzOne(bytes);
    }
    std::cout << "FUZZ_COMPLETED iterations=" << iterations << " seed=" << seed << "\n";
    return 0;
}
#endif
