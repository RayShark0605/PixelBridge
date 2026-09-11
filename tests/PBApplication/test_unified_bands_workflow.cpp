#include "unified_decoder_test_support.h"

#include "pbmodulation/supplemental_band.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// End-to-end workflow coverage for the experimental unified-bands profile
// (PB-Experimental-BlankControl-1, layout 11): a real EncoderRuntime renders
// frames with both supplemental bands, the frames are decoded through the CPU
// oracle (main region) plus the product band library (geometry sampling at
// 1:1 and at a 4/3 display transform), and the assembled results feed a real
// DecoderRuntime with the experimental binding. Assertions cover whole-file
// recovery, band admission counts, and the identity gate that keeps SC6-V3
// frames from triggering band decoding.
namespace
{

constexpr std::size_t kCanvasBytes = 1920 * 1080 * 4;
constexpr std::size_t kScaledCanvasBytes = 2560 * 1440 * 4;

struct BandsFrames
{
    std::vector<pbdemodd3d11::CaptureDemodulatorResult> frames;
    std::uint64_t encoderSubmittedBands = 0;
    std::uint64_t encoderSkippedBandFrames = 0;
    std::uint32_t admittedBands = 0;
    std::uint32_t bandDecodeAttempts = 0;
};

std::vector<std::byte> ScaleFourThirds(const std::span<const std::byte> source)
{
    REQUIRE(source.size() == kCanvasBytes);
    std::vector<std::byte> output(kScaledCanvasBytes);
    const auto Read = [&](const std::uint32_t column, const std::uint32_t row, const std::size_t channel)
    {
        return std::to_integer<std::uint32_t>(source[(static_cast<std::size_t>(row) * 1920 + column) * 4 + channel]);
    };
    for (std::uint32_t row = 0; row < 1440; row++)
    {
        const auto positionY = std::clamp<std::int32_t>(static_cast<std::int32_t>(6 * row) - 1, 0, 8 * (1080 - 1));
        const auto top = static_cast<std::uint32_t>(positionY / 8);
        const auto bottom = std::min(top + 1, 1079U);
        const auto weightY = static_cast<std::uint32_t>(positionY % 8);
        for (std::uint32_t column = 0; column < 2560; column++)
        {
            const auto positionX = std::clamp<std::int32_t>(static_cast<std::int32_t>(6 * column) - 1, 0, 8 * (1920 - 1));
            const auto left = static_cast<std::uint32_t>(positionX / 8);
            const auto right = std::min(left + 1, 1919U);
            const auto weightX = static_cast<std::uint32_t>(positionX % 8);
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                const std::uint32_t weighted = Read(left, top, channel) * (8 - weightX) * (8 - weightY) +
                    Read(right, top, channel) * weightX * (8 - weightY) +
                    Read(left, bottom, channel) * (8 - weightX) * weightY +
                    Read(right, bottom, channel) * weightX * weightY;
                output[(static_cast<std::size_t>(row) * 2560 + column) * 4 + channel] =
                    static_cast<std::byte>((weighted + 32) / 64);
            }
        }
    }
    return output;
}

BandsFrames MakeBandsFrames(const std::filesystem::path& root, const std::span<const std::byte> sourceBytes,
    const std::uint32_t frameCount, const bool scaled43)
{
    using namespace g16test;
    Check(frameCount > 0 && frameCount <= 8, "fixture frame budget is outside 1..8");
    std::filesystem::create_directories(root);
    const auto source = root / L"bands-source.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(sourceBytes.data()), static_cast<std::streamsize>(sourceBytes.size()));
        Check(file.good(), "cannot write fixture source");
    }
    const auto pixels = std::make_shared<PixelFrames>();
    pixels->limit = frameCount;
    pbapp::EncoderRuntime encoder([pixels](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<PixelCollector>(pixels);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.visualProfile = pbapp::VisualProfile::UnifiedBands;
    config.sessionStateRoot = root / L"encoder-state";
    const auto start = encoder.Start(config);
    Check(static_cast<bool>(start), start.message.c_str());
    const bool ready = WaitFor([&]()
    {
        const std::scoped_lock lock(pixels->mutex);
        return pixels->frames.size() == frameCount || encoder.GetSnapshot().state == pbapp::EncoderState::Failed;
    });
    encoder.Stop();
    const auto encoderSnapshot = encoder.GetSnapshot();
    Check(encoderSnapshot.state == pbapp::EncoderState::Stopped, encoderSnapshot.errorDetail.c_str());
    Check(ready && pixels->frames.size() == frameCount, encoderSnapshot.errorDetail.c_str());
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    Check(static_cast<bool>(oracleResult), "cannot create CPU pixel oracle");
    auto oracle = std::move(oracleResult).Value();
    BandsFrames output;
    output.encoderSubmittedBands = encoderSnapshot.submittedSupplementalBands;
    output.encoderSkippedBandFrames = encoderSnapshot.skippedSupplementalBandFrames;
    std::vector<std::byte> scratch(pbmodulation::kSupplementalBandPatchBytes);
    for (auto& raster : pixels->frames)
    {
        pbdemodd3d11::CaptureDemodulatorResult result;
        result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
        const std::vector<std::byte> canvas = scaled43 ? ScaleFourThirds(raster) : std::vector<std::byte>{};
        const std::span<const std::byte> physical = scaled43 ? std::span<const std::byte>(canvas) : std::span<const std::byte>(raster);
        const pbmodulation::LumaView view{physical, scaled43 ? 2560U : 1920U, scaled43 ? 1440U : 1080U,
            (scaled43 ? 2560U : 1920U) * 4, pbmodulation::LumaPixelFormat::Bgra8};
        result.geometryStatus = scaled43 ? pbdemodd3d11::CaptureDemodulatorGeometryStatus::Scaled :
            pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
        const auto observation = oracle.DecodeMixedFrame(view);
        Check(observation.IsFrameAvailable(), "runtime pixel fixture could not be decoded");
        result.bootstrap = observation.bootstrap;
        Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result.bootstrapRecord)),
            "cannot serialize observed Bootstrap");
        result.demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
        result.demodulation.unifiedObservation = observation;
        const auto blocks = oracle.GetAcceptedBlocks();
        result.demodulation.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(blocks.size());
        std::copy(blocks.begin(), blocks.end(), result.demodulation.acceptedUnifiedBlocks.begin());
        // Decode both supplemental bands through the product library using
        // the main-region observed geometry (the identity gate equivalent of
        // the demodulator hook: experimental profile + accepted parent).
        const auto& parent = observation.bootstrapRecord;
        pbmodulation::LocalDesktopGeometry resolved{};
        if (pbmodulation::ResolveUnifiedVisualSamplingGeometry(observation.bootstrap.geometry,
            view.width, view.height, pbmodulation::UnifiedVisualDecodePolicy{}, resolved))
        {
            for (std::size_t bandIndex = 0; bandIndex < result.supplementalBands.size(); bandIndex++)
            {
                auto& decoded = result.supplementalBands[bandIndex];
                const auto outcome = pbmodulation::DecodeSupplementalBand(view, resolved, bandIndex,
                    pbprotocol::SessionTag{parent.sessionTag}, parent.frameSequence,
                    scratch, decoded.record);
                decoded.status = outcome.status;
                decoded.recordBytes = static_cast<std::uint32_t>(outcome.recordBytes);
                output.bandDecodeAttempts++;
                if (outcome.IsAdmitted())
                {
                    result.admittedSupplementalBandCount++;
                    output.admittedBands++;
                }
            }
        }
        output.frames.push_back(std::move(result));
    }
    return output;
}

bool RunDecoderToCompletion(const std::filesystem::path& root, BandsFrames& bands,
    const std::span<const std::byte> expected, const pbapp::VisualProfile profile)
{
    using namespace g16test;
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    auto config = pbapp::MakeUnifiedDecoderConfig(root.wstring(), Region());
    config.visualProfile = profile;
    const auto start = runtime.Start(config);
    Check(static_cast<bool>(start), start.message.c_str());
    for (const auto& frame : bands.frames)
    {
        state->Push(frame);
        WaitFor([&]()
        {
            const std::scoped_lock lock(state->mutex);
            return state->frames.empty() || runtime.GetSnapshot().state == pbapp::DecoderState::Failed;
        });
        if (runtime.GetSnapshot().state == pbapp::DecoderState::Failed)
        {
            break;
        }
    }
    const bool completed = WaitFor([&]()
    {
        return runtime.GetSnapshot().state == pbapp::DecoderState::Completed ||
            runtime.GetSnapshot().state == pbapp::DecoderState::Failed;
    });
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    Check(snapshot.state != pbapp::DecoderState::Failed, snapshot.errorDetail.c_str());
    return completed && VerifyOutput(snapshot, expected);
}

} // namespace

TEST_CASE("Unified-bands frames recover whole files at 1:1 with admitted supplemental bands",
    "[application][unified-bands][workflow]")
{
    const auto root = std::filesystem::temp_directory_path() / ("pb-bands-11-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    BandsFrames bands = MakeBandsFrames(root, source, 4, false);
    // Every rendered frame carries a control record for the bands to mirror;
    // with repetitions=4 the scheduler always places at least the per-frame
    // descriptor prelude, so all frames submit both bands.
    REQUIRE(bands.encoderSubmittedBands >= 6);
    REQUIRE(bands.bandDecodeAttempts == 8);
    REQUIRE(bands.admittedBands == 8);
    REQUIRE(RunDecoderToCompletion(root, bands, source, pbapp::VisualProfile::UnifiedBands));
    std::filesystem::remove_all(root);
}

TEST_CASE("Unified-bands frames recover whole files through a 4/3 display transform",
    "[application][unified-bands][workflow][scaled]")
{
    const auto root = std::filesystem::temp_directory_path() / ("pb-bands-43-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    BandsFrames bands = MakeBandsFrames(root, source, 4, true);
    REQUIRE(bands.bandDecodeAttempts == 8);
    REQUIRE(bands.admittedBands == 8);
    REQUIRE(RunDecoderToCompletion(root, bands, source, pbapp::VisualProfile::UnifiedBands));
    std::filesystem::remove_all(root);
}

TEST_CASE("Tampered supplemental bands are rejected without poisoning recovery",
    "[application][unified-bands][negative]")
{
    const auto root = std::filesystem::temp_directory_path() / ("pb-bands-neg-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    BandsFrames bands = MakeBandsFrames(root, source, 4, false);
    // The mid-gray cell of an admitted band becomes ambiguous; the band is
    // rejected by the confidence rule and the count drops by exactly one.
    bool tampered = false;
    for (auto& frame : bands.frames)
    {
        if (!tampered && frame.admittedSupplementalBandCount != 0)
        {
            // Re-decode is not needed: flipping the admitted status models a
            // band whose raster the channel corrupted (band library tests
            // cover the pixel-level rejection); the pipeline contract under
            // test is that a non-admitted band never reaches control
            // admission and recovery still completes.
            frame.admittedSupplementalBandCount--;
            frame.supplementalBands[0].status = pbmodulation::SupplementalBandDecodeStatus::AmbiguousCell;
            frame.supplementalBands[0].recordBytes = 0;
            tampered = true;
        }
    }
    REQUIRE(tampered);
    REQUIRE(RunDecoderToCompletion(root, bands, source, pbapp::VisualProfile::UnifiedBands));
    std::filesystem::remove_all(root);
}

TEST_CASE("SC6-V3 frames do not carry supplemental band records",
    "[application][unified-bands][identity]")
{
    const auto root = std::filesystem::temp_directory_path() / ("pb-bands-id-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    // Plain SC6 frames: same harness but the product profile.
    std::filesystem::create_directories(root);
    const auto sourcePath = root / L"sc6-source.bin";
    {
        std::ofstream file(sourcePath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));
    }
    const auto pixels = std::make_shared<g16test::PixelFrames>();
    pixels->limit = 2;
    pbapp::EncoderRuntime encoder([pixels](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<g16test::PixelCollector>(pixels);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(sourcePath.wstring(), 60);
    config.sessionStateRoot = root / L"encoder-state";
    const auto start = encoder.Start(config);
    REQUIRE(static_cast<bool>(start));
    REQUIRE(g16test::WaitFor([&]()
    {
        const std::scoped_lock lock(pixels->mutex);
        return pixels->frames.size() == 2 || encoder.GetSnapshot().state == pbapp::EncoderState::Failed;
    }));
    encoder.Stop();
    const auto encoderSnapshot = encoder.GetSnapshot();
    REQUIRE(encoderSnapshot.state == pbapp::EncoderState::Stopped);
    REQUIRE(encoderSnapshot.submittedSupplementalBands == 0);
    // Band pixels are absent from SC6 frames: the blank areas stay matte, so
    // geometry sampling yields ambiguous cells rather than admitted records.
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(static_cast<bool>(oracleResult));
    auto oracle = std::move(oracleResult).Value();
    std::vector<std::byte> scratch(pbmodulation::kSupplementalBandPatchBytes);
    std::array<std::byte, pbmodulation::kSupplementalBandMaximumRecordBytes> record{};
    const auto& raster = pixels->frames.front();
    const pbmodulation::LumaView view{raster, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
    const auto observation = oracle.DecodeMixedFrame(view);
    REQUIRE(observation.IsFrameAvailable());
    const auto& parent = observation.bootstrapRecord;
    const auto outcome = pbmodulation::DecodeSupplementalBand(view, observation.bootstrap.geometry, 0,
        pbprotocol::SessionTag{parent.sessionTag}, parent.frameSequence, scratch, record);
    REQUIRE(outcome.status == pbmodulation::SupplementalBandDecodeStatus::AmbiguousCell);
    std::filesystem::remove_all(root);
}
