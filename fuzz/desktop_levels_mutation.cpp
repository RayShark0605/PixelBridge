#include "pbdesktoplevels/reference_channel.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace
{
void Check(const bool condition, const char* const reason)
{
    if (!condition)
    {
        std::cerr << "DESKTOP_LEVELS_MUTATION_INVARIANT " << reason << '\n';
        std::abort();
    }
}

struct Runner
{
    Runner() : pixels(pbmodulation::kLocalDesktopFrameBgraBytes), data(pbmodulation::kDesktopLevelsMaximumDataBytes),
        hard(pbmodulation::kDesktopLevelsMaximumDataBytes), soft(pbmodulation::kDesktopLevelsMaximumBits)
    {
        auto modulation = pbmodulation::DesktopLevelsWorkspace::Create(pbmodulation::DesktopLevelsWorkspace::RequiredBytes());
        auto reference = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        Check(modulation && reference, "startup workspace");
        workspace = std::move(modulation).Value();
        channel = std::move(reference).Value();
    }

    void Run(const std::span<const std::byte> input)
    {
        const auto Byte = [input](const std::size_t index)
        {
            return index < input.size() ? std::to_integer<unsigned>(input[index]) : 0u;
        };
        const auto id = Byte(0) % 2 == 0 ? pbmodulation::kDesktopLevels2ProfileId : pbmodulation::kDesktopLevels4ProfileId;
        const auto& profile = *pbmodulation::GetDesktopLevelsProfile(id);
        pbprotocol::BootstrapRecord record;
        record.visualLayoutVersion = pbmodulation::kDesktopLevelsLayoutVersion;
        record.protocolVersion = pbprotocol::GetProtocolVersion();
        record.visualProfileId = id;
        record.sessionTag.value = 0x1122334455667788ULL;
        record.frameSequence = Byte(1) % 16;
        std::array<std::byte, 44> bytes{};
        const auto logicalData = std::span(data).first(profile.dataBytes);
        Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(record, bytes)), "Bootstrap fixture");
        Check(static_cast<bool>(pbdesktoplevels::GenerateDiagnosticData(bytes, logicalData)), "Transport fixture");
        Check(static_cast<bool>(pbmodulation::EncodeDesktopLevelsFrame(bytes, logicalData, pixels)), "raster fixture");
        std::fill(hard.begin(), hard.end(), std::byte{0xA7});
        std::fill(soft.begin(), soft.end(), 123.25f);
        pbmodulation::LumaView view{pixels, 1920, 1080, 7680, pbmodulation::LumaPixelFormat::Bgra8};
        pbmodulation::DesktopLevelsDecodePolicy policy;
        auto hardOutput = std::span(hard);
        auto softOutput = std::span(soft);
        const unsigned action = Byte(2) % 12;
        if (action == 1 || action == 10)
        {
            pbmodulation::LocalDesktopRegion region;
            const auto physical = (Byte(3) * 256 + Byte(4)) % profile.tileCount;
            Check(pbmodulation::GetDesktopLevelsTile(id, physical, region), "tile fixture");
            Paint(region.x, region.y, action == 1 ? 1 : region.width, region.height, Byte(5));
        }
        else if (action == 2)
        {
            Paint(896 + (Byte(3) % 2) * 64, 16, 64, 64, Byte(4));
        }
        else if (action == 3)
        {
            Paint(736 + (Byte(3) % 4) * 32, 16, 32, 64, Byte(4));
        }
        else if (action == 4)
        {
            view.rowPitch = Byte(3) % 2 == 0 ? 7679 : std::numeric_limits<std::size_t>::max();
        }
        else if (action == 5)
        {
            hardOutput = hardOutput.first(profile.dataBytes - 1);
        }
        else if (action == 6)
        {
            Paint(96, 16, 608, 64, Byte(3));
        }
        else if (action == 7)
        {
            policy.maximumScaleDriftPixels = Byte(3) % 2 == 0 ? 0.126 : std::numeric_limits<double>::quiet_NaN();
        }
        else if (action == 8)
        {
            view.pixels = view.pixels.first(view.pixels.size() - 1 - Byte(3));
        }
        else if (action == 9)
        {
            policy.maximumDataWorkUnits = 1 + Byte(3) * 1024;
        }
        else if (action == 11)
        {
            view.pixelFormat = pbmodulation::LumaPixelFormat::R10G10B10A2;
        }
        const auto observation = pbmodulation::DecodeDesktopLevelsFrame(view, workspace, hardOutput, softOutput, policy);
        Check(observation.dataWorkUnits <= policy.maximumDataWorkUnits && observation.bootstrap.workUnits <= policy.locator.maximumWorkUnits, "bounded work");
        if (!observation.IsAccepted())
        {
            Check(std::ranges::all_of(hard, [](const auto value) { return value == std::byte{0xA7}; }), "erasure modified hard output");
            Check(std::ranges::all_of(soft, [](const auto value) { return value == 123.25f; }), "erasure modified soft output");
            Check(workspace.GetMarginHistogram().empty(), "erasure exposed stale histogram");
            Check(action != 0, "clean mutation seed erased");
            return;
        }
        Check(action != 4 && action != 5 && action != 7 && action != 8 && action != 9, "forbidden view/policy accepted");
        Check(observation.profileId == id && observation.dataBytes == profile.dataBytes, "wrong profile/length");
        Check(observation.margin.samples == profile.tileCount, "incomplete histogram");
        Check(observation.margin.minimum >= 0 && observation.margin.minimum <= 1, "invalid margin");
        const auto hardData = std::span(hard).first(profile.dataBytes);
        const auto metrics = std::span(soft).first(profile.dataBytes * 8ULL);
        Check(std::ranges::all_of(metrics, [](const float metric) { return std::isfinite(metric); }), "nonfinite metric");
        const auto evaluation = channel.EvaluateCodewords(observation.bootstrap.canonical44, hardData, metrics);
        Check(evaluation.evaluated, "accepted modulation not evaluable");
        // Only after all demod/FEC operations do we compare against fixture data.
        std::uint64_t errors = 0;
        for (std::size_t index = 0; index < profile.codewords * 2025ULL; index++)
        {
            errors += std::popcount(std::to_integer<unsigned>(hardData[index] ^ data[index]));
        }
        Check(evaluation.erroneousCodedBits == errors && evaluation.comparedCodedBits == profile.codewords * 16200ULL, "BER denominator/oracle mismatch");
        Check(evaluation.falseAcceptedCodewords == 0, "CRC-valid false acceptance");
        if (action == 0)
        {
            Check(evaluation.IsVerified() && errors == 0, "clean FEC seed failed");
        }
    }

    void Paint(const unsigned left, const unsigned top, const unsigned width, const unsigned height, const unsigned level)
    {
        Check(width <= 1920 - left && height <= 1080 - top, "mutation paint bound");
        for (unsigned row = top; row < top + height; row++)
        {
            for (unsigned column = left; column < left + width; column++)
            {
                const std::size_t offset = (static_cast<std::size_t>(row) * 1920 + column) * 4;
                pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = static_cast<std::byte>(level);
            }
        }
    }

    pbmodulation::DesktopLevelsWorkspace workspace;
    pbdesktoplevels::ReferenceChannel channel;
    std::vector<std::byte> pixels;
    std::vector<std::byte> data;
    std::vector<std::byte> hard;
    std::vector<float> soft;
};
} // namespace

int main(const int count, const char* const arguments[])
{
    Runner runner;
    if (count == 3 && std::string_view(arguments[1]) == "--input")
    {
        std::ifstream input(arguments[2], std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (!input || size < 0 || size > 4096)
        {
            std::cerr << "Corpus input must be 0..4096 bytes\n";
            return 2;
        }
        input.seekg(0);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            return 2;
        }
        runner.Run(bytes);
        std::cout << "DESKTOP_LEVELS_CORPUS_VALIDATED bytes=" << bytes.size() << '\n';
        return 0;
    }
    std::uint32_t iterations = 64;
    if (count == 2)
    {
        const std::string_view text(arguments[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            return 2;
        }
    }
    if (count > 2 || iterations == 0 || iterations > 512)
    {
        std::cerr << "Usage: PBDesktopLevelsMutation [1..512] | --input CORPUS.bin\n";
        return 2;
    }
    std::uint64_t state = 0x5042444C58314D55ULL;
    for (std::uint32_t iteration = 0; iteration < iterations; iteration++)
    {
        std::array<std::byte, 32> input{};
        for (auto& value : input)
        {
            // Intentional unsigned wrap: reproducible SplitMix64 PRNG, not
            // input size/offset/resource arithmetic.
            state += 0x9E3779B97F4A7C15ULL;
            auto mixed = state;
            mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
            mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
            value = static_cast<std::byte>((mixed ^ (mixed >> 31)) & 255);
        }
        input[0] = static_cast<std::byte>(iteration % 2);
        input[1] = static_cast<std::byte>((iteration / 2) % 16);
        input[2] = static_cast<std::byte>((iteration / 32) % 12);
        runner.Run(input);
    }
    std::cout << "DESKTOP_LEVELS_MUTATION_COMPLETED iterations=" << iterations << " seed=0x5042444C58314D55 deterministic=1\n";
    return 0;
}
