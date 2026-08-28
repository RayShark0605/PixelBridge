#include "local_desktop_test_fixtures.h"
#include "../../libs/PBModulation/src/local_desktop_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace
{

using pbmodulation::detail::BootstrapRsError;
using pbmodulation::detail::DecodeBootstrapRs;
using pbmodulation::detail::EncodeBootstrapRs;
using Record = std::array<std::byte, 44>;
using Codeword = std::array<std::byte, 76>;

// Deliberately not log/exp tables: independent polynomial arithmetic oracle.
std::uint8_t MultiplyOracle(const std::uint8_t left, const std::uint8_t right) noexcept
{
    std::uint16_t multiplicand = left;
    std::uint8_t multiplier = right;
    std::uint16_t product = 0;
    for (std::uint32_t bit = 0; bit < 8; bit++)
    {
        if ((multiplier & 1u) != 0)
        {
            product ^= multiplicand;
        }
        multiplier >>= 1;
        multiplicand <<= 1;
        if ((multiplicand & 0x100u) != 0)
        {
            multiplicand ^= 0x11Du;
        }
    }
    return static_cast<std::uint8_t>(product);
}

std::uint8_t PowerOracle(std::uint32_t exponent) noexcept
{
    std::uint8_t result = 1;
    std::uint8_t factor = 2;
    while (exponent != 0)
    {
        if ((exponent & 1u) != 0)
        {
            result = MultiplyOracle(result, factor);
        }
        factor = MultiplyOracle(factor, factor);
        exponent >>= 1;
    }
    return result;
}

Codeword EncodeOracle(const Record& record, const std::span<const std::byte> generator)
{
    REQUIRE(generator.size() == 33);
    std::array<std::uint8_t, 32> parity{};
    for (const auto symbol : record)
    {
        const auto feedback = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(symbol) ^ parity[0]);
        for (std::size_t index = 0; index < parity.size(); index++)
        {
            const auto following = index + 1 < parity.size() ? parity[index + 1] : std::uint8_t{0};
            parity[index] = static_cast<std::uint8_t>(following ^ MultiplyOracle(feedback, std::to_integer<std::uint8_t>(generator[index + 1])));
        }
    }
    Codeword result{};
    std::copy(record.begin(), record.end(), result.begin());
    for (std::size_t index = 0; index < parity.size(); index++)
    {
        result[record.size() + index] = static_cast<std::byte>(parity[index]);
    }
    return result;
}

std::uint32_t NextRandom(std::uint32_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

Codeword LoadCodeword(const std::string_view filename)
{
    const auto bytes = localdesktoptest::LoadGoldenBytes(filename, 76);
    Codeword result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

} // namespace

TEST_CASE("Bootstrap GF multiplication matches all 65536 independent polynomial products", "[pbmodulation][local-desktop][rs][oracle]")
{
    for (std::uint32_t left = 0; left < 256; left++)
    {
        for (std::uint32_t right = 0; right < 256; right++)
        {
            CAPTURE(left, right);
            REQUIRE(pbmodulation::detail::MultiplyBootstrapField(static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right)) ==
                    MultiplyOracle(static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right)));
        }
    }
}

TEST_CASE("Shortened RS matches independently generated wire bytes and accepts opaque data", "[pbmodulation][local-desktop][rs][golden]")
{
    const auto coefficients = localdesktoptest::LoadGoldenBytes("rs-generator.bin", 33);
    CHECK(localdesktoptest::ToHex(coefficients) == "01744034ae367e10c2a221219db0c5e10c3b37fde4942fb3b9188afd148e37ac58");
    for (const std::string_view filename : {"a-rs76.bin", "b-rs76.bin", "c-rs76.bin", "protocol-rs76.bin"})
    {
        CAPTURE(filename);
        const auto expected = LoadCodeword(filename);
        Record record{};
        std::copy_n(expected.begin(), record.size(), record.begin());
        Codeword encoded{};
        REQUIRE(EncodeBootstrapRs(record, encoded));
        CHECK(encoded == expected);
        CHECK(EncodeOracle(record, coefficients) == expected);
        Record decoded{};
        const auto status = DecodeBootstrapRs(expected, decoded);
        REQUIRE(status);
        CHECK(status.correctedSymbols == 0);
        CHECK(status.errorPosition == 0);
        CHECK(decoded == record);
    }
    const Record zeros{};
    const Codeword zeroWord{};
    Codeword encoded{};
    REQUIRE(EncodeBootstrapRs(zeros, encoded));
    CHECK(encoded == zeroWord);
    Record decoded{};
    decoded.fill(std::byte{0xCC});
    REQUIRE(DecodeBootstrapRs(zeroWord, decoded));
    CHECK(decoded == zeros);
}

TEST_CASE("RS corrects every nonzero byte error at every transmitted position", "[pbmodulation][local-desktop][rs][exhaustive]")
{
    const auto pristine = LoadCodeword("a-rs76.bin");
    const auto expected = localdesktoptest::LoadGoldenRecord();
    for (std::uint32_t position = 0; position < pristine.size(); position++)
    {
        for (std::uint32_t magnitude = 1; magnitude < 256; magnitude++)
        {
            CAPTURE(position, magnitude);
            auto corrupted = pristine;
            corrupted[position] ^= static_cast<std::byte>(magnitude);
            Record decoded{};
            const auto status = DecodeBootstrapRs(corrupted, decoded);
            REQUIRE(status);
            REQUIRE(status.correctedSymbols == 1);
            REQUIRE(decoded == expected);
        }
    }
}

TEST_CASE("RS corrects up to sixteen distinct errors in independent random codewords", "[pbmodulation][local-desktop][rs][boundaries]")
{
    const auto coefficients = localdesktoptest::LoadGoldenBytes("rs-generator.bin", 33);
    std::uint32_t random = 0x7058A19Du;
    for (std::uint32_t errors = 0; errors <= 16; errors++)
    {
        for (std::uint32_t trial = 0; trial < 48; trial++)
        {
            CAPTURE(errors, trial);
            Record record{};
            for (auto& symbol : record)
            {
                symbol = static_cast<std::byte>(NextRandom(random) & 255u);
            }
            const auto expectedWord = EncodeOracle(record, coefficients);
            Codeword producedWord{};
            REQUIRE(EncodeBootstrapRs(record, producedWord));
            REQUIRE(producedWord == expectedWord);
            auto corrupted = expectedWord;
            std::array<std::uint32_t, 76> positions{};
            for (std::uint32_t index = 0; index < positions.size(); index++)
            {
                positions[index] = index;
            }
            for (std::uint32_t index = 0; index < errors; index++)
            {
                const auto chosen = index + NextRandom(random) % (76u - index);
                std::swap(positions[index], positions[chosen]);
                corrupted[positions[index]] ^= static_cast<std::byte>(1u + NextRandom(random) % 255u);
            }
            Record decoded{};
            const auto status = DecodeBootstrapRs(corrupted, decoded);
            REQUIRE(status);
            REQUIRE(status.correctedSymbols == errors);
            REQUIRE(decoded == record);
        }
    }
}

TEST_CASE("RS rejects roots in both ends of the omitted shortening prefix", "[pbmodulation][local-desktop][rs][shortening]")
{
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    for (const bool last : {false, true})
    {
        const auto word = LoadCodeword(last ? "shortening-prefix-last.bin" : "shortening-prefix-first.bin");
        REQUIRE(std::equal(canonical.begin(), canonical.end(), word.begin()));
        for (const bool additionalError : {false, true})
        {
            CAPTURE(last, additionalError);
            auto corrupted = word;
            if (additionalError)
            {
                corrupted[75] ^= std::byte{0xE1};
            }
            Record output{};
            output.fill(std::byte{0xA5});
            const auto before = output;
            const auto status = DecodeBootstrapRs(corrupted, output);
            REQUIRE_FALSE(status);
            CHECK(status.error == BootstrapRsError::ShorteningViolation);
            CHECK(status.errorPosition == (last ? 178u : 0u));
            CHECK(status.correctedSymbols == 0);
            CHECK(output == before);
        }
    }
}

TEST_CASE("Seventeen parity errors with sixteen vanishing syndromes fail before payload commit", "[pbmodulation][local-desktop][rs][failure]")
{
    const auto invalid = LoadCodeword("uncorrectable-17.bin");
    const auto pristine = LoadCodeword("a-rs76.bin");
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    REQUIRE(std::equal(canonical.begin(), canonical.end(), invalid.begin()));
    std::size_t differences = 0;
    for (std::size_t index = 0; index < invalid.size(); index++)
    {
        differences += invalid[index] != pristine[index] ? 1u : 0u;
    }
    REQUIRE(differences == 17);
    Record output{};
    output.fill(std::byte{0x83});
    const auto before = output;
    const auto status = DecodeBootstrapRs(invalid, output);
    REQUIRE_FALSE(status);
    CHECK(status.error == BootstrapRsError::LocatorDegree);
    CHECK(status.correctedSymbols == 0);
    CHECK(output == before);
}

TEST_CASE("An independently rootless degree-two locator fails exactly at root count without committing or altering aliases", "[pbmodulation][local-desktop][rs][rootless]")
{
    const auto invalid = LoadCodeword("rootless-locator.bin");
    const auto pristine = LoadCodeword("a-rs76.bin");
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    REQUIRE(localdesktoptest::ToHex(invalid) == "504252470101000231305342444c4250d0ba97d94b20df81181716151413121124232221000000009910265691f0d9731ab12fe67aa53429aba6ce3bd68f6eafadb5f054b941af89ac35a2b8");
    REQUIRE(std::equal(canonical.begin(), canonical.end(), invalid.begin()));
    std::size_t differences = 0;
    for (std::size_t index = 0; index < invalid.size(); index++)
    {
        differences += invalid[index] != pristine[index] ? 1u : 0u;
    }
    REQUIRE(differences == 32);

    constexpr std::uint8_t delta = 0x20;
    std::uint8_t trace = 0;
    auto conjugate = delta;
    for (std::uint32_t index = 0; index < 8; index++)
    {
        trace ^= conjugate;
        conjugate = MultiplyOracle(conjugate, conjugate);
    }
    REQUIRE(trace == 1);
    std::uint32_t roots = 0;
    for (std::uint32_t candidate = 0; candidate < 256; candidate++)
    {
        const auto value = static_cast<std::uint8_t>(candidate);
        const auto polynomial = static_cast<std::uint8_t>(1u ^ value ^ MultiplyOracle(delta, MultiplyOracle(value, value)));
        roots += polynomial == 0 ? 1u : 0u;
    }
    REQUIRE(roots == 0);

    std::array<std::uint8_t, 32> expectedSyndromes{1, 0};
    for (std::size_t index = 2; index < expectedSyndromes.size(); index++)
    {
        expectedSyndromes[index] = static_cast<std::uint8_t>(expectedSyndromes[index - 1] ^ MultiplyOracle(delta, expectedSyndromes[index - 2]));
    }
    // Lambda=[1,1,delta] is an exact degree-two recurrence. Degree one is
    // impossible: S0=1,S1=0 implies coefficient zero, whereas S2=delta!=0.
    REQUIRE(expectedSyndromes[0] == 1);
    REQUIRE(expectedSyndromes[1] == 0);
    REQUIRE(expectedSyndromes[2] == delta);
    for (std::uint32_t root = 0; root < 32; root++)
    {
        std::uint8_t actual = 0;
        for (std::uint32_t position = 0; position < 76; position++)
        {
            actual ^= MultiplyOracle(std::to_integer<std::uint8_t>(invalid[position]), PowerOracle(root * (75u - position)));
        }
        CAPTURE(root);
        REQUIRE(actual == expectedSyndromes[root]);
    }

    Record output{};
    output.fill(std::byte{0x6B});
    const auto before = output;
    const auto status = DecodeBootstrapRs(invalid, output);
    REQUIRE_FALSE(status);
    CHECK(status.error == BootstrapRsError::LocatorRootCount);
    CHECK(status.errorPosition == 0);
    CHECK(status.correctedSymbols == 0);
    CHECK(output == before);

    constexpr std::array<std::array<std::size_t, 2>, 4> placements{{{0, 0}, {0, 16}, {0, 32}, {16, 0}}};
    for (const auto& placement : placements)
    {
        const auto inputOffset = placement[0];
        const auto outputOffset = placement[1];
        CAPTURE(inputOffset, outputOffset);
        std::array<std::byte, 128> overlapped{};
        overlapped.fill(std::byte{0xD3});
        std::copy(invalid.begin(), invalid.end(), overlapped.begin() + static_cast<std::ptrdiff_t>(inputOffset));
        const auto original = overlapped;
        const auto aliased = DecodeBootstrapRs(std::span(overlapped).subspan(inputOffset, 76), std::span(overlapped).subspan(outputOffset, 44));
        REQUIRE_FALSE(aliased);
        CHECK(aliased.error == BootstrapRsError::LocatorRootCount);
        CHECK(aliased.errorPosition == 0);
        CHECK(aliased.correctedSymbols == 0);
        CHECK(overlapped == original);
    }
}

TEST_CASE("RS malformed lengths and all failures preserve outputs including aliasing", "[pbmodulation][local-desktop][rs][atomicity]")
{
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    const auto codeword = LoadCodeword("a-rs76.bin");
    std::array<std::byte, 96> storage{};
    storage.fill(std::byte{0xAD});
    const auto before = storage;
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{43}, std::size_t{45}})
    {
        CAPTURE(length);
        const auto status = EncodeBootstrapRs(std::span(storage).first(length), std::span(storage).first(76));
        CHECK(status.error == BootstrapRsError::InvalidInputSize);
        CHECK(storage == before);
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{75}, std::size_t{77}})
    {
        CAPTURE(length);
        CHECK(EncodeBootstrapRs(canonical, std::span(storage).first(length)).error == BootstrapRsError::InvalidOutputSize);
        CHECK(storage == before);
        CHECK(DecodeBootstrapRs(std::span(storage).first(length), std::span(storage).first(44)).error == BootstrapRsError::InvalidInputSize);
        CHECK(storage == before);
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{43}, std::size_t{45}})
    {
        CAPTURE(length);
        CHECK(DecodeBootstrapRs(codeword, std::span(storage).first(length)).error == BootstrapRsError::InvalidOutputSize);
        CHECK(storage == before);
    }
    for (const std::string_view filename : {"shortening-prefix-first.bin", "shortening-prefix-last.bin", "uncorrectable-17.bin"})
    {
        CAPTURE(filename);
        auto overlapped = LoadCodeword(filename);
        const auto original = overlapped;
        REQUIRE_FALSE(DecodeBootstrapRs(overlapped, std::span(overlapped).subspan(16, 44)));
        CHECK(overlapped == original);
    }
}

TEST_CASE("RS success permits overlapping input and output in either direction", "[pbmodulation][local-desktop][rs][aliasing]")
{
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    const auto expectedWord = LoadCodeword("a-rs76.bin");
    for (const std::size_t offset : {std::size_t{0}, std::size_t{16}, std::size_t{32}})
    {
        CAPTURE(offset);
        std::array<std::byte, 128> buffer{};
        buffer.fill(std::byte{0xC5});
        std::copy(canonical.begin(), canonical.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        REQUIRE(EncodeBootstrapRs(std::span(buffer).subspan(offset, 44), std::span(buffer).first(76)));
        CHECK(std::equal(expectedWord.begin(), expectedWord.end(), buffer.begin()));
        CHECK(buffer[127] == std::byte{0xC5});
        buffer[0] ^= std::byte{0x27};
        buffer[75] ^= std::byte{0xFE};
        const auto status = DecodeBootstrapRs(std::span(buffer).first(76), std::span(buffer).subspan(offset, 44));
        REQUIRE(status);
        CHECK(status.correctedSymbols == 2);
        CHECK(std::equal(canonical.begin(), canonical.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset)));
        CHECK(buffer[127] == std::byte{0xC5});
    }
    std::array<std::byte, 128> shifted{};
    std::copy(canonical.begin(), canonical.end(), shifted.begin());
    REQUIRE(EncodeBootstrapRs(std::span(shifted).first(44), std::span(shifted).subspan(16, 76)));
    CHECK(std::equal(expectedWord.begin(), expectedWord.end(), shifted.begin() + 16));
    REQUIRE(DecodeBootstrapRs(std::span(shifted).subspan(16, 76), std::span(shifted).first(44)));
    CHECK(std::equal(canonical.begin(), canonical.end(), shifted.begin()));
}

TEST_CASE("Arbitrary received RS words commit only an independently valid nearby shortened codeword", "[pbmodulation][local-desktop][rs][adversarial]")
{
    const auto generator = localdesktoptest::LoadGoldenBytes("rs-generator.bin", 33);
    std::uint32_t random = 0x830BA47Du;
    std::uint32_t failures = 0;
    for (std::uint32_t trial = 0; trial < 1024; trial++)
    {
        CAPTURE(trial);
        Codeword received{};
        for (auto& symbol : received)
        {
            symbol = static_cast<std::byte>(NextRandom(random) & 255u);
        }
        Record output{};
        output.fill(std::byte{0xF6});
        const auto before = output;
        const auto status = DecodeBootstrapRs(received, output);
        if (!status)
        {
            failures++;
            REQUIRE(output == before);
            REQUIRE(status.correctedSymbols == 0);
            continue;
        }
        const auto independentlyEncoded = EncodeOracle(output, generator);
        std::uint32_t distance = 0;
        for (std::size_t index = 0; index < received.size(); index++)
        {
            distance += received[index] != independentlyEncoded[index] ? 1u : 0u;
        }
        REQUIRE(distance <= 16);
        REQUIRE(status.correctedSymbols == distance);
    }
    REQUIRE(failures != 0);
}

TEST_CASE("RS tables and scratch state are independent across concurrent callers", "[pbmodulation][local-desktop][rs][threads]")
{
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    const auto word = LoadCodeword("a-rs76.bin");
    std::atomic<std::uint32_t> failures{0};
    std::array<std::jthread, 4> threads;
    for (std::uint32_t worker = 0; worker < threads.size(); worker++)
    {
        threads[worker] = std::jthread([&, worker]
        {
            for (std::uint32_t iteration = 0; iteration < 128; iteration++)
            {
                auto corrupted = word;
                for (std::uint32_t error = 0; error < 16; error++)
                {
                    corrupted[(worker + iteration + error * 3) % 76] ^= static_cast<std::byte>(error + 1);
                }
                Record decoded{};
                const auto status = DecodeBootstrapRs(corrupted, decoded);
                Codeword encoded{};
                if (!status || status.correctedSymbols != 16 || decoded != canonical || !EncodeBootstrapRs(canonical, encoded) || encoded != word)
                {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK(failures.load(std::memory_order_relaxed) == 0);
}
