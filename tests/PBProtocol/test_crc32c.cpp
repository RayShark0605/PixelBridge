#include "pbprotocol/crc32c.h"
#include "crc32c_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace {

[[nodiscard]] std::span<const std::byte> AsBytes(
    const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Deterministic pattern for the streaming equivalence checks.
[[nodiscard]] std::vector<std::byte> MakePattern(const std::size_t byteCount)
{
    std::vector<std::byte> data;
    data.reserve(byteCount);
    for (std::uint32_t index = 0; index < byteCount; index++)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(index * 7u + 13u);
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

[[nodiscard]] std::uint32_t ComputeChunked(
    const std::span<const std::byte> data,
    const std::size_t chunkSize)
{
    pbprotocol::Crc32c hasher;
    for (std::size_t offset = 0; offset < data.size(); offset += chunkSize)
    {
        const std::size_t remaining = data.size() - offset;
        const std::size_t count = std::min(chunkSize, remaining);
        hasher.Update(data.subspan(offset, count));
    }
    return hasher.Finalize();
}

[[nodiscard]] std::uint32_t UpdateBitwiseReference(std::uint32_t state, const std::span<const std::byte> data) noexcept
{
    for (const std::byte value : data)
    {
        state ^= std::to_integer<std::uint32_t>(value);
        for (int bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            state = (state >> 1) ^ ((state & 1u) != 0u ? 0x82F63B78u : 0u);
        }
    }
    return state;
}

#if defined(_WIN32)
struct VirtualAllocationDeleter
{
    void operator()(std::byte* allocation) const noexcept
    {
        if (allocation != nullptr)
        {
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
    }
};
#endif

} // namespace

TEST_CASE("CRC-32C matches the published check values",
          "[pbprotocol][crc32c][kat]")
{
    REQUIRE(pbprotocol::ComputeCrc32c({}) == 0x00000000u);
    REQUIRE(pbprotocol::ComputeCrc32c(AsBytes("123456789")) == 0xE3069283u);

    // Single zero byte, verified against an independent bit-level reference.
    const std::array<std::byte, 1> zeroByte{std::byte{0x00}};
    REQUIRE(pbprotocol::ComputeCrc32c(zeroByte) == 0x527D5351u);
}

TEST_CASE("CRC-32C streaming equals one-shot for arbitrary chunking",
          "[pbprotocol][crc32c][streaming]")
{
    const auto pattern = MakePattern(4096);
    const std::uint32_t reference = pbprotocol::ComputeCrc32c(pattern);

    const std::size_t chunkSizes[] = {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 63u, 64u, 65u, 256u, 257u};
    for (const std::size_t chunkSize : chunkSizes)
    {
        REQUIRE(ComputeChunked(pattern, chunkSize) == reference);
    }
}

TEST_CASE("CRC-32C empty updates are no-ops and Finalize is repeatable",
          "[pbprotocol][crc32c][streaming]")
{
    pbprotocol::Crc32c hasher;
    const std::uint32_t emptyValue = hasher.Finalize();
    REQUIRE(emptyValue == 0x00000000u);

    hasher.Update({});
    REQUIRE(hasher.Finalize() == emptyValue);

    // Continuation after Finalize equals one-shot over the concatenation.
    const auto first = MakePattern(1000);
    const auto second = MakePattern(777);
    std::vector<std::byte> concatenated;
    concatenated.reserve(first.size() + second.size());
    for (const std::byte element : first)
    {
        concatenated.push_back(element);
    }
    for (const std::byte element : second)
    {
        concatenated.push_back(element);
    }

    hasher.Update(first);
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeCrc32c(first));
    hasher.Update(second);
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeCrc32c(concatenated));
}

TEST_CASE("CRC-32C dispatch and portable reference match independent bitwise alignment and tail cases",
    "[pbprotocol][crc32c][reference][boundary]")
{
    auto pattern = MakePattern(1048576 + 32);
    std::uint32_t randomState = 0x9834a75bu;
    for (auto& value : pattern)
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        value = static_cast<std::byte>(randomState & 0xFFu);
    }
    for (std::size_t offset = 0; offset < 32; offset++)
    {
        CAPTURE(offset);
        for (std::size_t count = 0; count <= 129; count++)
        {
            CAPTURE(count);
            const auto bytes = std::span<const std::byte>(pattern).subspan(offset, count);
            const auto expected = UpdateBitwiseReference(0xFFFFFFFFu, bytes) ^ 0xFFFFFFFFu;
            REQUIRE(pbprotocol::ComputeCrc32c(bytes) == expected);
            REQUIRE((pbprotocol::detail::UpdateCrc32cPortable(0xFFFFFFFFu, bytes) ^ 0xFFFFFFFFu) == expected);
        }
        for (const std::size_t count : {1023u, 1024u, 1025u, 1629u, 4095u, 4096u, 4097u, 65535u, 65536u, 1048576u})
        {
            CAPTURE(count);
            const auto bytes = std::span<const std::byte>(pattern).subspan(offset, count);
            const auto expected = UpdateBitwiseReference(0xFFFFFFFFu, bytes) ^ 0xFFFFFFFFu;
            REQUIRE(pbprotocol::ComputeCrc32c(bytes) == expected);
            REQUIRE((pbprotocol::detail::UpdateCrc32cPortable(0xFFFFFFFFu, bytes) ^ 0xFFFFFFFFu) == expected);
            REQUIRE(ComputeChunked(bytes, 33) == expected);
        }
    }
    for (const std::uint32_t state : {0u, 1u, 0x80000000u, 0xFFFFFFFFu, 0x32178BC9u})
    {
        const auto bytes = std::span<const std::byte>(pattern).first(1629);
        REQUIRE(pbprotocol::detail::UpdateCrc32cPortable(state, bytes) == UpdateBitwiseReference(state, bytes));
    }
}

#if defined(_WIN32)
TEST_CASE("CRC-32C does not read beyond either end of a guarded input span",
    "[pbprotocol][crc32c][boundary][guard-page]")
{
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const std::size_t pageBytes = information.dwPageSize;
    REQUIRE(pageBytes >= 4096);
    REQUIRE(pageBytes <= 65536);
    std::unique_ptr<std::byte, VirtualAllocationDeleter> allocation(static_cast<std::byte*>(VirtualAlloc(nullptr, pageBytes * 3, MEM_RESERVE, PAGE_NOACCESS)));
    REQUIRE(allocation != nullptr);
    auto* const bytes = static_cast<std::byte*>(VirtualAlloc(allocation.get() + pageBytes, pageBytes, MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(bytes != nullptr);
    const auto pattern = MakePattern(pageBytes);
    std::copy(pattern.begin(), pattern.end(), bytes);
    for (std::size_t count = 0; count <= 257; count++)
    {
        CAPTURE(count);
        for (const std::size_t offset : {std::size_t{0}, pageBytes - count})
        {
            CAPTURE(offset);
            const auto span = std::span<const std::byte>(bytes + offset, count);
            const auto expected = UpdateBitwiseReference(0xFFFFFFFFu, span) ^ 0xFFFFFFFFu;
            REQUIRE(pbprotocol::ComputeCrc32c(span) == expected);
            REQUIRE((pbprotocol::detail::UpdateCrc32cPortable(0xFFFFFFFFu, span) ^ 0xFFFFFFFFu) == expected);
        }
    }
    const auto page = std::span<const std::byte>(bytes, pageBytes);
    REQUIRE(pbprotocol::ComputeCrc32c(page) == (UpdateBitwiseReference(0xFFFFFFFFu, page) ^ 0xFFFFFFFFu));
}
#endif
