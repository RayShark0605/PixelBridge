#include "pbprotocol/crc32c.h"

#include <array>

namespace pbprotocol {

namespace {

constexpr std::uint32_t kCrc32cReflectedPolynomial = 0x82F63B78u;
constexpr std::uint32_t kCrc32cFinalXor = 0xFFFFFFFFu;

// Reflected (LSB-first) table entry for one seed byte. The generated table
// matches the published CRC-32C check value ("123456789" -> 0xE3069283).
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

const std::array<std::uint32_t, 256>& GetCrc32cTable() noexcept
{
    static const std::array<std::uint32_t, 256> table = BuildTable();
    return table;
}

} // namespace

void Crc32c::Update(const std::span<const std::byte> data) noexcept
{
    if (data.empty())
    {
        return;
    }

    const auto& table = GetCrc32cTable();
    for (const std::byte element : data)
    {
        const std::uint8_t byteValue = std::to_integer<std::uint8_t>(element);
        crc_ = (crc_ >> 8) ^ table[(crc_ ^ byteValue) & 0xFFu];
    }
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