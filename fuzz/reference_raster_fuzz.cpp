// PBModulation reference-raster fuzz driver (design 38.2 Phase-0 gates).
//
// Clang/libFuzzer builds (PB_USE_LIBFUZZER=1) expose only
// LLVMFuzzerTestOneInput. Other backends build a deterministic mutation
// runner with the same ExerciseInput core:
//
//   PBModulationReferenceRasterFuzz [iterations] [seed]
//   PBModulationReferenceRasterFuzz --input <corpus-file>
//
// The mutation loop is seeded, so every CTest smoke run replays the exact
// same input sequence. Full-size 8294400-byte frames are synthesized in
// memory from a valid canonical base frame plus random corruptions; they
// are deliberately not committed to the corpus (repo size).

#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using pbmodulation::DecodePngFrame;
using pbmodulation::DecodeRawFrame;
using pbmodulation::DecodeReferenceFrame;
using pbmodulation::EncodeRawFrame;
using pbmodulation::ParseReferenceRegionManifest;
using pbmodulation::ReferenceFrameInput;
using pbmodulation::EncodeReferenceFrame;

constexpr std::size_t kManifestBytes =
    pbmodulation::kReferenceManifestBytes;
constexpr std::size_t kFrameBytes = pbmodulation::kReferenceFrameBgraBytes;
constexpr std::size_t kMaximumSmallInputBytes = 16384;
constexpr std::size_t kMaximumPngProbeDimension = 64;
constexpr std::size_t kMaximumFrameMutations = 4;
constexpr std::size_t kMaximumMutationRunBytes = 64;
constexpr std::uint64_t kDefaultIterations = 1000;
constexpr std::uint64_t kDefaultSeed = 0x50424D4F44550001ULL;

// SplitMix64: the same deterministic PRNG family the test helpers use,
// so mutation sequences are reproducible across runs and machines.
struct SplitMix64
{
    std::uint64_t state;

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

std::vector<std::byte> gValidBaseFrame;

[[nodiscard]] bool EnsureValidBaseFrame()
{
    if (!gValidBaseFrame.empty())
    {
        return true;
    }
    // A canonical frame with an all-zero payload is a valid input to the
    // raster encoder (the raster layer is content-agnostic), and it is the
    // cheapest full-size base to corrupt.
    const std::vector<std::byte> zeroData(
        pbmodulation::kReferenceDataRegionBytes, std::byte{0});
    ReferenceFrameInput input;
    input.controlWindow =
        std::span<const std::byte>(zeroData.begin(),
            pbmodulation::kReferenceControlWindowBytes);
    input.data = zeroData;
    std::vector<std::byte> frame(kFrameBytes, std::byte{0});
    const auto status =
        EncodeReferenceFrame(input, std::span<std::byte>(frame));
    if (!status)
    {
        return false;
    }
    gValidBaseFrame = std::move(frame);
    return true;
}

// Exercises the raw container decode boundary. The out span is sized from
// the (untrusted) header only when every field passes checked validation;
// hostile headers therefore never drive an allocation.
void ExerciseRawDecode(const std::span<const std::byte> raw)
{
    if (raw.size() >= pbmodulation::kRawFrameHeaderBytes)
    {
        const auto readUint32 = [&raw](const std::size_t offset)
            -> std::uint64_t
        {
            std::uint64_t value = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                value |= static_cast<std::uint64_t>(
                    std::to_integer<std::uint8_t>(raw[offset + shift / 8]))
                    << shift;
            }
            return value;
        };
        const std::uint64_t width = readUint32(8);
        const std::uint64_t height = readUint32(12);
        const std::uint64_t pixelBytes = readUint32(16);
        const bool dimensionsWithinBound =
            width >= 1 && width <= pbmodulation::kMaximumFrameDimension &&
            height >= 1 &&
            height <= pbmodulation::kMaximumFrameDimension;
        const bool pixelBytesConsistent =
            dimensionsWithinBound &&
            pixelBytes == width * height * 4 &&
            pixelBytes + pbmodulation::kRawFrameHeaderBytes <=
                raw.size();
        if (pixelBytesConsistent)
        {
            std::vector<std::byte> bgra(
                static_cast<std::size_t>(pixelBytes));
            std::uint32_t outWidth = 0;
            std::uint32_t outHeight = 0;
            (void)DecodeRawFrame(
                raw, std::span<std::byte>(bgra), outWidth, outHeight);
            return;
        }
    }
    // Otherwise let the decoder reject the header against a minimal out
    // span; the header validation must run before any size acceptance.
    // Mutable scratch buffer: DecodeRawFrame takes a non-const out span and
    // only writes into it on success (the rejected path never touches it).
    std::array<std::byte, 4> probe{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    std::uint32_t outWidth = 0;
    std::uint32_t outHeight = 0;
    (void)DecodeRawFrame(
        raw, std::span<std::byte>(probe), outWidth, outHeight);
}

void ExercisePngDecode(const std::span<const std::byte> png)
{
    if (png.size() < 8)
    {
        return;
    }
    // Derive a bounded expectation from the first two bytes so every
    // input exercises the geometry-mismatch and decode-error paths; the
    // corpus valid-png-small matches the expectation for signature bytes
    // 0x89, 0x50 (10x17).
    const std::uint32_t expectedWidth =
        1 + std::to_integer<std::uint8_t>(png[0]) %
            kMaximumPngProbeDimension;
    const std::uint32_t expectedHeight =
        1 + std::to_integer<std::uint8_t>(png[1]) %
            kMaximumPngProbeDimension;
    const std::size_t bgraBytes =
        static_cast<std::size_t>(expectedWidth) * expectedHeight * 4;
    std::vector<std::byte> bgra(bgraBytes);
    (void)DecodePngFrame(
        png, expectedWidth, expectedHeight, std::span<std::byte>(bgra));
}

// Exercises the encoder input validation with a random-sized span.
void ExerciseRawEncode(const std::span<const std::byte> input)
{
    if (input.empty() || input.size() % 4 != 0)
    {
        return;
    }
    const std::uint32_t width =
        static_cast<std::uint32_t>(
            std::min<std::size_t>(input.size() / 4,
                pbmodulation::kMaximumFrameDimension));
    (void)EncodeRawFrame(input, width, 1);
}

void ExerciseInput(const std::span<const std::byte> input)
{
    if (input.size() == kManifestBytes)
    {
        (void)ParseReferenceRegionManifest(input);
        return;
    }
    if (input.size() == kFrameBytes)
    {
        (void)DecodeReferenceFrame(input);
        return;
    }
    ExerciseRawDecode(input);
    ExercisePngDecode(input);
    ExerciseRawEncode(input);
}

[[nodiscard]] bool ParseUint64(
    const char* const text,
    std::uint64_t& outValue) noexcept
{
    std::string_view value(text);
    if (value.empty())
    {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long result = std::strtoull(text, &end, 10);
    if (end != text + value.size() || result == 0ULL ||
        (errno == ERANGE && result == ULLONG_MAX))
    {
        return false;
    }
    outValue = static_cast<std::uint64_t>(result);
    return true;
}

int RunMutationLoop(const std::uint64_t iterations, const std::uint64_t seed)
{
    if (!EnsureValidBaseFrame())
    {
        std::cerr << "internal: base frame encode failed\n";
        return 3;
    }
    SplitMix64 random(seed);
    std::vector<std::byte> workingFrame;
    std::vector<std::byte> smallInput;
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        const std::uint64_t mode = random.Next() % 100;
        if (mode < 50)
        {
            // Corrupt a copy of the valid full frame: a few random runs of
            // random bytes, plus occasional single-pixel flips in frozen
            // regions.
            workingFrame = gValidBaseFrame;
            const std::size_t mutationCount =
                1 + static_cast<std::size_t>(
                    random.Next() % kMaximumFrameMutations);
            for (std::size_t mutationIndex = 0;
                mutationIndex < mutationCount; mutationIndex++)
            {
                const std::size_t offset =
                    static_cast<std::size_t>(
                        random.Next() % workingFrame.size());
                const std::size_t runBytes =
                    1 + static_cast<std::size_t>(
                        random.Next() % kMaximumMutationRunBytes);
                for (std::size_t byteIndex = 0;
                    byteIndex < runBytes &&
                    offset + byteIndex < workingFrame.size();
                    byteIndex++)
                {
                    workingFrame[offset + byteIndex] = std::byte{
                        static_cast<std::uint8_t>(
                            random.Next() & 0xFFu)};
                }
            }
            ExerciseInput(std::span<const std::byte>(workingFrame));
        }
        else if (mode < 80)
        {
            const std::size_t size =
                static_cast<std::size_t>(
                    random.Next() % (kMaximumSmallInputBytes + 1));
            smallInput.resize(size);
            for (auto& value : smallInput)
            {
                value = std::byte{
                    static_cast<std::uint8_t>(random.Next() & 0xFFu)};
            }
            ExerciseInput(std::span<const std::byte>(smallInput));
        }
        else if (mode < 95)
        {
            // Random manifest-size input (most fail the magic/CRC gate).
            std::vector<std::byte> manifestInput(kManifestBytes);
            for (auto& value : manifestInput)
            {
                value = std::byte{
                    static_cast<std::uint8_t>(random.Next() & 0xFFu)};
            }
            ExerciseInput(std::span<const std::byte>(manifestInput));
        }
        else
        {
            // Truncated frame: exercises the raw/PNG boundary dispatch at
            // full-frame-adjacent sizes.
            const std::size_t size =
                static_cast<std::size_t>(
                    random.Next() % (kFrameBytes + 1));
            ExerciseInput(
                std::span<const std::byte>(gValidBaseFrame).first(size));
        }
    }
    return 0;
}

int ReplayInputFile(const char* const path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::cerr << "cannot open corpus file: " << path << "\n";
        return 2;
    }
    // Named iterators: the direct-argument form would be parsed as a
    // function declaration (most vexing parse). std::byte is only
    // explicitly constructible from char, so convert element by element.
    const std::istreambuf_iterator<char> fileBegin(file);
    const std::istreambuf_iterator<char> fileEnd;
    std::vector<std::byte> input;
    for (auto it = fileBegin; it != fileEnd; it++)
    {
        input.push_back(static_cast<std::byte>(*it));
    }
    ExerciseInput(std::span<const std::byte>(input));
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* const data,
    const std::size_t size)
{
    ExerciseInput(std::as_bytes(std::span(data, size)));
    return 0;
}

#if !defined(PB_USE_LIBFUZZER)

int main(const int argumentCount, char* arguments[])
{
    if (argumentCount == 3 &&
        std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }

    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(arguments[1], iterations))
    {
        std::cerr << "usage: PBModulationReferenceRasterFuzz "
                     "[iterations] [seed]\n"
                     "       PBModulationReferenceRasterFuzz --input "
                     "<corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBModulationReferenceRasterFuzz "
                     "[iterations] [seed]\n"
                     "       PBModulationReferenceRasterFuzz --input "
                     "<corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBModulationReferenceRasterFuzz "
                     "[iterations] [seed]\n"
                     "       PBModulationReferenceRasterFuzz --input "
                     "<corpus-file>\n";
        return 2;
    }
    return RunMutationLoop(iterations, seed);
}

#endif
