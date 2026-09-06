#include "local_desktop_runtime.h"
#include "sender_carousel_scheduler.h"
#include "pbprotocol/blake3_digest.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

// Extracted by tests/UnifiedRemoteGate/extract_g21_phase_loss.py. Journal SHA-256:
// 77d5e91dd51d50d9d6c11d7872b2153dbda86b22f197d52e3216fdab89d380d2.
// These bands are a conservative projection, not recovered capture timestamps.
constexpr std::array<std::uint32_t, 107> erasedBands{
    5, 6, 12, 13, 28, 29, 31, 32, 42, 43, 45, 46, 52, 53, 66, 67, 72, 73, 78, 79, 90, 91, 98, 99,
    107, 108, 112, 113, 117, 128, 133, 134, 144, 145, 155, 156, 157, 158, 165, 166, 173, 174, 200, 201,
    216, 217, 218, 219, 225, 226, 239, 242, 265, 266, 276, 277, 279, 280, 288, 289, 322, 323, 331, 332,
    359, 360, 366, 371, 383, 391, 392, 395, 396, 401, 402, 407, 408, 410, 411, 417, 418, 422, 423,
    428, 429, 442, 443, 452, 453, 454, 455, 460, 461, 463, 464, 475, 481, 482, 487, 488, 489, 493,
    494, 497, 502, 504, 510};

void WriteSource(const std::filesystem::path& path)
{
    std::ofstream file(path, std::ios::binary);
    std::vector<char> buffer(1024U * 1024U);
    for (std::size_t index = 0; index < buffer.size(); index++)
    {
        buffer[index] = static_cast<char>((index * 157U + index / 251U + 11U) & 255U);
    }
    for (std::uint32_t chunk = 0; chunk < 128; chunk++)
    {
        file.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }
    file.close();
    if (!file)
    {
        throw std::runtime_error("deterministic source write failed");
    }
}

[[nodiscard]] std::array<std::byte, 32> HashFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    pbprotocol::Blake3Hasher hasher;
    std::vector<std::byte> buffer(1024U * 1024U);
    while (file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0)
    {
        hasher.Update(std::span(buffer).first(static_cast<std::size_t>(file.gcount())));
    }
    if (!file.eof())
    {
        throw std::runtime_error("independent file read failed");
    }
    return hasher.Finalize();
}

void WriteArray(std::ostream& output, const std::span<const std::uint64_t> values)
{
    output << '[';
    for (std::size_t index = 0; index < values.size(); index++)
    {
        output << (index == 0 ? "" : ",") << values[index];
    }
    output << ']';
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    try
    {
        if (argumentCount != 3)
        {
            throw std::runtime_error("usage: PBUnifiedWindowTransitionProbe NEW_ROOT clean|0..7");
        }
        const std::filesystem::path root = std::filesystem::absolute(arguments[1]).lexically_normal();
        const std::wstring mode = arguments[2];
        if (mode != L"clean" && (mode.size() != 1 || mode.front() < L'0' || mode.front() > L'7'))
        {
            throw std::runtime_error("invalid bounded phase");
        }
        if (!std::filesystem::create_directory(root))
        {
            throw std::runtime_error("evidence root already exists");
        }
        const auto source = root / L"source-128mib.bin";
        const auto outputDirectory = root / L"receiver";
        std::filesystem::create_directory(outputDirectory);
        WriteSource(source);
        pbapp::UnifiedWindowTransitionProbeSnapshot result;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedWindowTransition(source.wstring(),
            root / L"sender-state", outputDirectory.wstring(), mode == L"clean" ?
                std::span<const std::uint32_t>{} : std::span<const std::uint32_t>(erasedBands),
            mode == L"clean" ? 0U : static_cast<std::uint32_t>(mode.front() - L'0'), result);
        const bool externalDigestEqual = result.published &&
            std::filesystem::file_size(result.publishedPath) == result.sourceBytes &&
            HashFile(source) == HashFile(result.publishedPath);
        bool noRecoveryResidue = result.published;
        for (const auto& entry : std::filesystem::directory_iterator(outputDirectory))
        {
            noRecoveryResidue &= entry.path().extension() != L".part" && entry.path().extension() != L".resume";
        }
        const bool allSegmentsReachedK = result.acceptedSymbols.size() == 16 &&
            std::ranges::all_of(result.acceptedSymbols, [](const std::uint64_t count) { return count >= 6385; });
        const bool passed = status && result.transitionSafe && result.firstWindowFullCaptureBytesPerFrame >= 16384.0 &&
            result.noRasterEncodedBytesPerObservedFrame >= 16384.0 && externalDigestEqual && noRecoveryResidue && allSegmentsReachedK;
        std::ofstream report(root / L"result.json");
        report << std::boolalpha << std::setprecision(17)
            << "{\n\"schema\":\"PixelBridge.G21.Pass0WindowTransition.1\",\n"
            << "\"authority\":\"Deterministic no-raster production scheduler/storage/journal; not live pixels\",\n"
            << "\"mode\":" << std::quoted(mode == L"clean" ? std::string("clean") : std::string(1, static_cast<char>(mode.front()))) << ",\n"
            << "\"status\":" << static_cast<bool>(status) << ",\n\"error\":" << std::quoted(status.message) << ",\n"
            << "\"passed\":" << passed << ",\n\"repairDenominator\":" << pbapp::senderUnifiedInitialRepairPercentDenominator
            << ",\n\"phaseHoldSweeps\":" << pbapp::senderUnifiedSweepPhaseHold
            << ",\n\"sourceBytes\":" << result.sourceBytes << ",\n\"senderFrames\":" << result.senderFrames
            << ",\n\"observedFrames\":" << result.observedFrames << ",\n\"firstWindowFrames\":" << result.firstWindowFrames
            << ",\n\"firstWindowCompletedSegments\":" << result.firstWindowCompletedSegments
            << ",\n\"activeDecodersBeforeTransition\":" << result.activeDecodersBeforeTransition
            << ",\n\"transitionSafe\":" << result.transitionSafe << ",\n\"completedSegments\":" << result.completedSegments
            << ",\n\"peakActiveDecoders\":" << result.peakActiveDecoders
            << ",\n\"peakReservedDecoderBytes\":" << result.peakReservedDecoderBytes
            << ",\n\"resourceRejections\":" << result.resourceRejections << ",\n\"conflictRejections\":" << result.conflictRejections
            << ",\n\"deferredCount\":" << result.deferredCount << ",\n\"quotaCount\":" << result.quotaCount
            << ",\n\"orphanCount\":" << result.orphanCount << ",\n\"checkpointUpdates\":" << result.checkpointUpdates
            << ",\n\"firstWindowFullCaptureBytesPerFrame\":" << result.firstWindowFullCaptureBytesPerFrame
            << ",\n\"noRasterEncodedBytesPerObservedFrame\":";
        if (result.published && result.wholeDigestVerified && result.finalReopenVerified)
        {
            report << result.noRasterEncodedBytesPerObservedFrame;
        }
        else
        {
            report << "null";
        }
        report << ",\n\"metricUnavailableReason\":" << std::quoted(result.published ? "" : "NotPublished")
            << ",\n\"noRecoveryResidue\":" << noRecoveryResidue << ",\n\"allSegmentsReachedK\":" << allSegmentsReachedK
            << ",\n\"sourceStable\":" << (status ? (result.sourceStable ? "true" : "false") : "null")
            << ",\n\"durableLeaseAheadOfUse\":" << result.durableLeaseAheadOfUse
            << ",\n\"wholeDigestVerified\":" << result.wholeDigestVerified << ",\n\"published\":" << result.published
            << ",\n\"finalReopenVerified\":" << result.finalReopenVerified << ",\n\"externalDigestEqual\":" << externalDigestEqual
            << ",\n\"acceptedSymbols\":";
        WriteArray(report, result.acceptedSymbols);
        report << ",\n\"completedAtFrame\":";
        WriteArray(report, result.completedAtFrame);
        report << "\n}\n";
        report.close();
        if (!report)
        {
            throw std::runtime_error("result report write failed");
        }
        std::cout << "passed=" << passed << " transition=" << result.transitionSafe << " completed=" << result.completedSegments
            << " firstWindowFrames=" << result.firstWindowFrames << " fullCaptureBudget=" << result.firstWindowFullCaptureBytesPerFrame
            << " error=" << status.message << '\n';
        return passed ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 2;
    }
}
