#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/frame_io.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
void Emit(const pbdesktoplevels::ReferenceObservation& observation)
{
    std::cout << "{\"event\":\"cpu-observation\",\"backend\":\"cpu-reference\",\"modulation\":";
    pbdesktoplevels::WriteModulationJson(std::cout, observation.modulation);
    std::cout << ",\"evaluation\":";
    pbdesktoplevels::WriteEvaluationJson(std::cout, observation.evaluation);
    std::cout << "}\n";
}

int Baseline(pbdesktoplevels::ReferenceChannel& channel)
{
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    for (const auto profileId : {pbmodulation::kDesktopLevels2ProfileId, pbmodulation::kDesktopLevels4ProfileId})
    {
        const auto& profile = *pbmodulation::GetDesktopLevelsProfile(profileId);
        std::vector<std::byte> data(profile.dataBytes);
        pbdesktoplevels::ReferenceStatistics statistics;
        for (std::uint64_t sequence = 0; sequence < 16; sequence++)
        {
            pbprotocol::BootstrapRecord record;
            record.visualLayoutVersion = pbmodulation::kDesktopLevelsLayoutVersion;
            record.protocolVersion = pbprotocol::GetProtocolVersion();
            record.visualProfileId = profileId;
            record.sessionTag.value = 0x1122334455667788ULL;
            record.frameSequence = sequence;
            std::array<std::byte, 44> canonical{};
            if (!pbprotocol::SerializeBootstrapRecord(record, canonical) || !pbdesktoplevels::GenerateDiagnosticData(canonical, data) ||
                !pbmodulation::EncodeDesktopLevelsFrame(canonical, data, pixels))
            {
                throw std::runtime_error("CPU sender generation failed");
            }
            // Receiver receives pixels only, not record/data/phase/slot hints.
            const auto observation = channel.Decode({pixels, 1920, 1080, 7680, pbmodulation::LumaPixelFormat::Bgra8});
            Emit(observation);
            const auto recovered = pbprotocol::ParseBootstrapRecord(observation.modulation.bootstrap.canonical44);
            if (!recovered || !observation.modulation.IsAccepted() || !observation.evaluation.IsVerified() || observation.evaluation.erroneousCodedBits != 0 ||
                !statistics.Add(observation.evaluation, recovered.Value().frameSequence, channel.GetMarginHistogram(), observation.modulation.margin.minimum))
            {
                throw std::runtime_error("CPU baseline failed exact recovery");
            }
        }
        const auto summary = statistics.GetSummary();
        std::cout << "{\"event\":\"cpu-summary\",\"backend\":\"cpu-reference\",\"candidate\":\"desktop-levels-"
                  << profile.tilePixels << 'x' << profile.tilePixels << "\",\"metrics\":";
        pbdesktoplevels::WriteStatisticsJson(std::cout, summary);
        std::cout << "}\n";
        if (summary.frames != 16 || summary.verifiedFrames != 16 || summary.verifiedPhases != 65535)
        {
            throw std::runtime_error("CPU sample/phase evidence incomplete");
        }
    }
    return 0;
}

int Inspect(pbdesktoplevels::ReferenceChannel& channel, const std::filesystem::path& path)
{
    // Explicit offline input budget, separate from the 16 MiB processor. A
    // hostile container cannot allocate from dimensions before its file bound.
    constexpr std::uint64_t maximumInputBytes = 64 * 1024 * 1024;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (!input || length < static_cast<std::streamoff>(pbmodulation::kRawFrameHeaderBytes) || length > static_cast<std::streamoff>(maximumInputBytes))
    {
        throw std::runtime_error("PBRW input missing or outside 28..67108864 byte bound");
    }
    input.seekg(0);
    std::vector<std::byte> raw(static_cast<std::size_t>(length));
    if (!input.read(reinterpret_cast<char*>(raw.data()), length))
    {
        throw std::runtime_error("PBRW read failed");
    }
    std::vector<std::byte> pixels(raw.size() - pbmodulation::kRawFrameHeaderBytes);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!pbmodulation::DecodeRawFrame(raw, pixels, width, height))
    {
        throw std::runtime_error("PBRW validation failed");
    }
    const auto observation = channel.Decode({pixels, width, height, static_cast<std::size_t>(width) * 4, pbmodulation::LumaPixelFormat::Bgra8});
    Emit(observation);
    if (observation.evaluation.falseAcceptedCodewords != 0)
    {
        return 1;
    }
    return observation.evaluation.IsVerified() ? 0 : 4;
}
} // namespace

#ifdef _WIN32
int wmain(const int count, const wchar_t* const arguments[])
#else
int main(const int count, const char* const arguments[])
#endif
{
    try
    {
        if (!((count == 2 && std::filesystem::path(arguments[1]) == "--baseline") || (count == 3 && std::filesystem::path(arguments[1]) == "--input")))
        {
            std::cerr << "Usage: PBDesktopLevelsBaseline --baseline | --input FRAME.pbrw\n";
            return 2;
        }
        std::cout.imbue(std::locale::classic());
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        if (!created)
        {
            throw std::runtime_error("Processor workspace/Robust codec creation failed");
        }
        auto channel = std::move(created).Value();
        const int result = count == 2 ? Baseline(channel) : Inspect(channel, std::filesystem::path(arguments[2]));
        std::cout.flush();
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DesktopLevels baseline error: " << error.what() << '\n';
        return 1;
    }
}
