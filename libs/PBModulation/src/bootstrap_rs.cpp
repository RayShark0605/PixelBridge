#include "local_desktop_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation::detail
{
namespace
{

struct FieldTables
{
    std::array<std::uint8_t, 512> powers{};
    std::array<std::uint8_t, 256> logarithms{};
};

constexpr FieldTables BuildFieldTables() noexcept
{
    FieldTables tables;
    std::uint16_t value = 1;
    for (std::uint32_t exponent = 0; exponent < 255; exponent++)
    {
        tables.powers[exponent] = static_cast<std::uint8_t>(value);
        tables.logarithms[value] = static_cast<std::uint8_t>(exponent);
        value = static_cast<std::uint16_t>(value << 1);
        if ((value & 0x100u) != 0)
        {
            value ^= kBootstrapRsFieldPolynomial;
        }
    }
    for (std::size_t exponent = 255; exponent < tables.powers.size(); exponent++)
    {
        tables.powers[exponent] = tables.powers[exponent - 255];
    }
    return tables;
}

constexpr FieldTables field = BuildFieldTables();

constexpr std::uint8_t Multiply(const std::uint8_t left, const std::uint8_t right) noexcept
{
    return left == 0 || right == 0 ? 0 : field.powers[static_cast<std::size_t>(field.logarithms[left]) + field.logarithms[right]];
}

// All callers establish a nonzero denominator: the last nonzero BM
// discrepancy, or a pivot explicitly selected as nonzero below.
constexpr std::uint8_t Divide(const std::uint8_t numerator, const std::uint8_t denominator) noexcept
{
    return numerator == 0 ? 0 : field.powers[static_cast<std::size_t>(field.logarithms[numerator]) + 255u - field.logarithms[denominator]];
}

constexpr std::array<std::uint8_t, kLocalDesktopRsParityBytes + 1> BuildGenerator() noexcept
{
    std::array<std::uint8_t, kLocalDesktopRsParityBytes + 1> generator{};
    generator[0] = 1;
    for (std::size_t root = 0; root < kLocalDesktopRsParityBytes; root++)
    {
        std::array<std::uint8_t, kLocalDesktopRsParityBytes + 1> next{};
        for (std::size_t coefficient = 0; coefficient <= root; coefficient++)
        {
            next[coefficient] ^= generator[coefficient];
            next[coefficient + 1] ^= Multiply(generator[coefficient], field.powers[root]);
        }
        generator = next;
    }
    return generator;
}

constexpr auto generator = BuildGenerator();
static_assert(generator[0] == 1 && generator[1] == 0x74 && generator[32] == 0x58);
static_assert(Multiply(field.powers[254], 2) == 1);

using ReceivedWord = std::array<std::uint8_t, kLocalDesktopRsCodewordBytes>;
using Syndromes = std::array<std::uint8_t, kLocalDesktopRsParityBytes>;
using Locator = std::array<std::uint8_t, kLocalDesktopRsParityBytes + 1>;

Syndromes ComputeSyndromes(const ReceivedWord& word) noexcept
{
    Syndromes syndromes{};
    for (std::size_t root = 0; root < syndromes.size(); root++)
    {
        std::uint8_t value = 0;
        for (const auto symbol : word)
        {
            value = static_cast<std::uint8_t>(Multiply(value, field.powers[root]) ^ symbol);
        }
        syndromes[root] = value;
    }
    return syndromes;
}

bool AllZero(const Syndromes& syndromes) noexcept
{
    return std::ranges::all_of(syndromes, [](const std::uint8_t value) { return value == 0; });
}

BootstrapRsStatus FindLocator(const Syndromes& syndromes, Locator& locator, std::uint32_t& degree) noexcept
{
    locator[0] = 1;
    Locator previous{};
    previous[0] = 1;
    std::uint32_t shift = 1;
    std::uint8_t previousDiscrepancy = 1;
    for (std::uint32_t iteration = 0; iteration < syndromes.size(); iteration++)
    {
        std::uint8_t discrepancy = syndromes[iteration];
        for (std::uint32_t coefficient = 1; coefficient <= degree; coefficient++)
        {
            discrepancy ^= Multiply(locator[coefficient], syndromes[iteration - coefficient]);
        }
        if (discrepancy == 0)
        {
            shift++;
            continue;
        }
        const auto saved = locator;
        const auto scale = Divide(discrepancy, previousDiscrepancy);
        for (std::size_t coefficient = 0; coefficient + shift < locator.size(); coefficient++)
        {
            locator[coefficient + shift] ^= Multiply(scale, previous[coefficient]);
        }
        if (2u * degree <= iteration)
        {
            degree = iteration + 1u - degree;
            if (degree > kBootstrapRsMaximumErrors)
            {
                return {BootstrapRsError::LocatorDegree};
            }
            previous = saved;
            previousDiscrepancy = discrepancy;
            shift = 1;
        }
        else
        {
            shift++;
        }
    }
    return degree == 0 || locator[degree] == 0 ? BootstrapRsStatus{BootstrapRsError::LocatorDegree} : BootstrapRsStatus{};
}

struct ErrorLocation
{
    std::uint32_t transmittedPosition = 0;
    std::uint8_t fieldLocation = 0;
};

BootstrapRsStatus FindLocations(const Locator& locator, const std::uint32_t degree,
                               std::array<ErrorLocation, kBootstrapRsMaximumErrors>& locations) noexcept
{
    std::uint32_t count = 0;
    // Scan the FULL codeword, not just the 76 transmitted symbols. A decoder
    // must never "repair" one of the omitted, known-zero prefix symbols.
    for (std::uint32_t position = 0; position < kBootstrapRsFullSymbols; position++)
    {
        const auto exponent = kBootstrapRsFullSymbols - 1u - position;
        const auto inverse = field.powers[(255u - exponent) % 255u];
        std::uint8_t value = locator[degree];
        for (std::uint32_t coefficient = degree; coefficient > 0; coefficient--)
        {
            value = static_cast<std::uint8_t>(Multiply(value, inverse) ^ locator[coefficient - 1]);
        }
        if (value != 0)
        {
            continue;
        }
        if (position < kBootstrapRsShorteningSymbols)
        {
            return {BootstrapRsError::ShorteningViolation, 0, position};
        }
        if (count >= degree || count == locations.size())
        {
            return {BootstrapRsError::LocatorRootCount};
        }
        locations[count] = {position - kBootstrapRsShorteningSymbols, field.powers[exponent]};
        count++;
    }
    return count == degree ? BootstrapRsStatus{} : BootstrapRsStatus{BootstrapRsError::LocatorRootCount};
}

BootstrapRsStatus CorrectMagnitudes(ReceivedWord& word, const Syndromes& syndromes,
                                   const std::array<ErrorLocation, kBootstrapRsMaximumErrors>& locations, const std::uint32_t count) noexcept
{
    // Solve S_j = sum(error_i * X_i^j) over GF(256), j=0..count-1.
    // This bounded <=16x17 Vandermonde solve avoids Forney convention ambiguity
    // for the fixed first root 0. Distinct Chien locations give unique X_i.
    std::array<std::array<std::uint8_t, kBootstrapRsMaximumErrors + 1>, kBootstrapRsMaximumErrors> matrix{};
    for (std::uint32_t column = 0; column < count; column++)
    {
        std::uint8_t value = 1;
        for (std::uint32_t row = 0; row < count; row++)
        {
            matrix[row][column] = value;
            value = Multiply(value, locations[column].fieldLocation);
        }
    }
    for (std::uint32_t row = 0; row < count; row++)
    {
        matrix[row][count] = syndromes[row];
    }
    for (std::uint32_t column = 0; column < count; column++)
    {
        std::uint32_t pivot = column;
        while (pivot < count && matrix[pivot][column] == 0)
        {
            pivot++;
        }
        if (pivot == count)
        {
            return {BootstrapRsError::MagnitudeFailure};
        }
        if (pivot != column)
        {
            std::swap(matrix[pivot], matrix[column]);
        }
        const auto divisor = matrix[column][column];
        for (std::uint32_t index = column; index <= count; index++)
        {
            matrix[column][index] = Divide(matrix[column][index], divisor);
        }
        for (std::uint32_t row = 0; row < count; row++)
        {
            if (row == column)
            {
                continue;
            }
            const auto factor = matrix[row][column];
            for (std::uint32_t index = column; index <= count; index++)
            {
                matrix[row][index] ^= Multiply(factor, matrix[column][index]);
            }
        }
    }
    for (std::uint32_t index = 0; index < count; index++)
    {
        const auto magnitude = matrix[index][count];
        if (magnitude == 0)
        {
            return {BootstrapRsError::MagnitudeFailure};
        }
        word[locations[index].transmittedPosition] ^= magnitude;
    }
    return {};
}

} // namespace

std::uint8_t MultiplyBootstrapField(const std::uint8_t left, const std::uint8_t right) noexcept
{
    return Multiply(left, right);
}

BootstrapRsStatus EncodeBootstrapRs(const std::span<const std::byte> record, const std::span<std::byte> codeword) noexcept
{
    if (record.size() != kLocalDesktopBootstrapRecordBytes)
    {
        return {BootstrapRsError::InvalidInputSize};
    }
    if (codeword.size() != kLocalDesktopRsCodewordBytes)
    {
        return {BootstrapRsError::InvalidOutputSize};
    }
    ReceivedWord division{};
    std::array<std::byte, kLocalDesktopRsCodewordBytes> encoded{};
    for (std::size_t index = 0; index < record.size(); index++)
    {
        division[index] = std::to_integer<std::uint8_t>(record[index]);
        encoded[index] = record[index];
    }
    for (std::size_t index = 0; index < record.size(); index++)
    {
        const auto leading = division[index];
        for (std::size_t coefficient = 0; coefficient < generator.size(); coefficient++)
        {
            division[index + coefficient] ^= Multiply(leading, generator[coefficient]);
        }
    }
    for (std::size_t index = record.size(); index < division.size(); index++)
    {
        encoded[index] = static_cast<std::byte>(division[index]);
    }
    std::copy(encoded.begin(), encoded.end(), codeword.begin());
    return {};
}

BootstrapRsStatus DecodeBootstrapRs(const std::span<const std::byte> codeword, const std::span<std::byte> record) noexcept
{
    if (codeword.size() != kLocalDesktopRsCodewordBytes)
    {
        return {BootstrapRsError::InvalidInputSize};
    }
    if (record.size() != kLocalDesktopBootstrapRecordBytes)
    {
        return {BootstrapRsError::InvalidOutputSize};
    }
    ReceivedWord corrected{};
    for (std::size_t index = 0; index < codeword.size(); index++)
    {
        corrected[index] = std::to_integer<std::uint8_t>(codeword[index]);
    }
    const auto syndromes = ComputeSyndromes(corrected);
    std::uint32_t degree = 0;
    if (!AllZero(syndromes))
    {
        Locator locator{};
        auto status = FindLocator(syndromes, locator, degree);
        if (!status)
        {
            return status;
        }
        std::array<ErrorLocation, kBootstrapRsMaximumErrors> locations{};
        status = FindLocations(locator, degree, locations);
        if (!status)
        {
            return status;
        }
        status = CorrectMagnitudes(corrected, syndromes, locations, degree);
        if (!status)
        {
            return status;
        }
        if (!AllZero(ComputeSyndromes(corrected)))
        {
            return {BootstrapRsError::SyndromeMismatch};
        }
    }
    for (std::size_t index = 0; index < record.size(); index++)
    {
        record[index] = static_cast<std::byte>(corrected[index]);
    }
    return {BootstrapRsError::None, degree};
}

} // namespace pbmodulation::detail
