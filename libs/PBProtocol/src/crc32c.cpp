#include "pbprotocol/crc32c.h"
#include "crc32c_internal.h"

#include <array>
#include <cstring>

// Clang-cl also defines _MSC_VER but requires separate per-function ISA
// attributes. Keep unvalidated compiler backends on the portable reference.
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

namespace pbprotocol {

namespace {

constexpr std::uint32_t kCrc32cReflectedPolynomial = 0x82F63B78u;
constexpr std::uint32_t kCrc32cFinalXor = 0xFFFFFFFFu;

constexpr std::uint32_t BuildTableEntry(const std::uint32_t seed) noexcept
{
    std::uint32_t entry = seed;
    for (int bitIndex = 0; bitIndex < 8; bitIndex++)
    {
        if ((entry & 1u) != 0u)
        {
            entry = (entry >> 1) ^ kCrc32cReflectedPolynomial;
        }
        else
        {
            entry >>= 1;
        }
    }
    return entry;
}

constexpr std::array<std::uint32_t, 256> BuildTable() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); index++)
    {
        table[index] = BuildTableEntry(static_cast<std::uint32_t>(index));
    }
    return table;
}

constexpr auto kCrc32cTable = BuildTable();

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
bool DetectSse42() noexcept
{
    std::array<int, 4> cpuInformation{};
    __cpuid(cpuInformation.data(), 0);
    if (cpuInformation[0] < 1)
    {
        return false;
    }
    __cpuid(cpuInformation.data(), 1);
    return (static_cast<std::uint32_t>(cpuInformation[2]) & (1u << 20)) != 0u;
}

// Keep optional instructions behind the runtime gate, including under LTCG.
__declspec(noinline) std::uint32_t UpdateCrc32cSse42(const std::uint32_t state, const std::span<const std::byte> data) noexcept
{
    std::uint64_t runningState = state;
    std::size_t offset = 0;
    while (data.size() - offset >= sizeof(std::uint64_t))
    {
        std::uint64_t word = 0;
        // x64 is little-endian. memcpy permits unaligned input without aliasing
        // violations, and every load remains entirely inside the input span.
        std::memcpy(&word, data.data() + offset, sizeof(word));
        runningState = _mm_crc32_u64(runningState, word);
        offset += sizeof(word);
    }
    std::uint32_t remainingState = static_cast<std::uint32_t>(runningState);
    if (data.size() - offset >= sizeof(std::uint32_t))
    {
        std::uint32_t word = 0;
        std::memcpy(&word, data.data() + offset, sizeof(word));
        remainingState = _mm_crc32_u32(remainingState, word);
        offset += sizeof(word);
    }
    for (; offset < data.size(); offset++)
    {
        remainingState = _mm_crc32_u8(remainingState, std::to_integer<std::uint8_t>(data[offset]));
    }
    return remainingState;
}
#endif

} // namespace

namespace detail {

std::uint32_t UpdateCrc32cPortable(std::uint32_t state, const std::span<const std::byte> data) noexcept
{
    for (const std::byte element : data)
    {
        const std::uint8_t byteValue = std::to_integer<std::uint8_t>(element);
        state = (state >> 8) ^ kCrc32cTable[(state ^ byteValue) & 0xFFu];
    }
    return state;
}

bool IsCrc32cHardwareAvailable() noexcept
{
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    static const bool available = DetectSse42();
    return available;
#else
    return false;
#endif
}

} // namespace detail

void Crc32c::Update(const std::span<const std::byte> data) noexcept
{
    if (data.empty())
    {
        return;
    }
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    if (detail::IsCrc32cHardwareAvailable())
    {
        crc_ = UpdateCrc32cSse42(crc_, data);
        return;
    }
#endif
    crc_ = detail::UpdateCrc32cPortable(crc_, data);
}

std::uint32_t Crc32c::Finalize() const noexcept
{
    return crc_ ^ kCrc32cFinalXor;
}

std::uint32_t ComputeCrc32c(const std::span<const std::byte> data) noexcept
{
    Crc32c hasher;
    hasher.Update(data);
    return hasher.Finalize();
}

} // namespace pbprotocol
