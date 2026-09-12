#include "unified_decoder_test_support.h"

#include "pbprotocol/product_visual_profile.h"

#include <array>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// End-to-end workflow coverage for the experimental unified-gray profile
// (PB-Experimental-GrayStates-1, layout 12): a real EncoderRuntime renders
// gray-state frames, the frames are decoded through the CPU oracle at 1:1 and
// through an idealized 4:2:0 chroma-subsampled channel (the transform that
// erases the SC6-V3 Chroma carrier outright), and the assembled results feed a
// real DecoderRuntime with the experimental binding. Assertions cover the
// wire identity, whole-file recovery through the chroma-destroying channel,
// and the family gate that keeps the product profile untouched.
namespace
{

constexpr std::size_t kCanvasBytes = 1920 * 1080 * 4;

std::vector<std::byte> SubsampleChroma420(const std::span<const std::byte> source)
{
    REQUIRE(source.size() == kCanvasBytes);
    std::vector<std::byte> output(source.size());
    std::copy(source.begin(), source.end(), output.begin());
    for (std::uint32_t blockRow = 0; blockRow < 1080 / 2; blockRow++)
    {
        for (std::uint32_t blockColumn = 0; blockColumn < 1920 / 2; blockColumn++)
        {
            double opponentBlueSum = 0;
            double opponentRedSum = 0;
            std::array<double, 4> luma{};
            std::size_t sample = 0;
            for (std::uint32_t row = 0; row < 2; row++)
            {
                for (std::uint32_t column = 0; column < 2; column++)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(blockRow * 2 + row) * 1920 + blockColumn * 2 + column) * 4;
                    const double blue = std::to_integer<std::uint8_t>(output[index]);
                    const double green = std::to_integer<std::uint8_t>(output[index + 1]);
                    const double red = std::to_integer<std::uint8_t>(output[index + 2]);
                    luma[sample] = 0.0722 * blue + 0.7152 * green + 0.2126 * red;
                    opponentBlueSum += blue - luma[sample];
                    opponentRedSum += red - luma[sample];
                    sample++;
                }
            }
            const double opponentBlueMean = opponentBlueSum / 4.0;
            const double opponentRedMean = opponentRedSum / 4.0;
            sample = 0;
            for (std::uint32_t row = 0; row < 2; row++)
            {
                for (std::uint32_t column = 0; column < 2; column++)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(blockRow * 2 + row) * 1920 + blockColumn * 2 + column) * 4;
                    const double blue = luma[sample] + opponentBlueMean;
                    const double red = luma[sample] + opponentRedMean;
                    const double green = luma[sample] -
                        (0.0722 * opponentBlueMean + 0.2126 * opponentRedMean) / 0.7152;
                    output[index] = static_cast<std::byte>(std::lround(std::clamp(blue, 0.0, 255.0)));
                    output[index + 1] = static_cast<std::byte>(std::lround(std::clamp(green, 0.0, 255.0)));
                    output[index + 2] = static_cast<std::byte>(std::lround(std::clamp(red, 0.0, 255.0)));
                    sample++;
                }
            }
        }
    }
    return output;
}

struct GrayFrames
{
    std::vector<pbdemodd3d11::CaptureDemodulatorResult> frames;
    std::uint64_t decodedTransportBlocks = 0;
    std::uint64_t chromaLaneErasedFrames = 0;
};

GrayFrames MakeGrayFrames(const std::filesystem::path& root, const std::span<const std::byte> sourceBytes,
    const std::uint32_t frameCount, const bool chromaSubsampled)
{
    using namespace g16test;
    Check(frameCount > 0 && frameCount <= 8, "fixture frame budget is outside 1..8");
    std::filesystem::create_directories(root);
    const auto source = root / L"gray-source.bin";
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
    config.visualProfile = pbapp::VisualProfile::UnifiedGray;
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
    GrayFrames output;
    for (auto& raster : pixels->frames)
    {
        const std::vector<std::byte> physical = chromaSubsampled ? SubsampleChroma420(raster) : raster;
        const pbmodulation::LumaView view{physical, 1920U, 1080U, 1920U * 4, pbmodulation::LumaPixelFormat::Bgra8};
        // The CPU oracle expects the gray-state wire identity, mirroring the
        // binding the product demodulator resolves from the Bootstrap record.
        pbmodulation::UnifiedExpectedFrameIdentity expected;
        expected.visualProfileId = pbprotocol::kGrayStatesExperimentalProfile.visualProfileId;
        expected.visualLayoutVersion = pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion;
        const auto observation = oracle.DecodeMixedFrame(view, expected);
        Check(observation.IsFrameAvailable(), "gray-state pixel fixture could not be decoded");
        // Lock the wire identity: layout 12 / experimental profile id on the
        // Bootstrap itself, so a binding regression cannot hide behind the
        // product profile while the gray mechanics still pass.
        Check(observation.bootstrapRecord.visualProfileId == pbprotocol::kGrayStatesExperimentalProfile.visualProfileId &&
            observation.bootstrapRecord.visualLayoutVersion == pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion,
            "unified-gray frames must carry the experimental Bootstrap identity");
        Check(observation.chroma.IsAvailable(), "the gray state lane must stay live through the channel");
        output.decodedTransportBlocks += observation.acceptedTransportBlocks;
        output.chromaLaneErasedFrames += observation.chroma.IsAvailable() ? 0 : 1;
        pbdemodd3d11::CaptureDemodulatorResult result;
        result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
        result.geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
        result.bootstrap = observation.bootstrap;
        Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result.bootstrapRecord)),
            "cannot serialize observed Bootstrap");
        result.demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
        result.demodulation.unifiedObservation = observation;
        const auto blocks = oracle.GetAcceptedBlocks();
        result.demodulation.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(blocks.size());
        std::copy(blocks.begin(), blocks.end(), result.demodulation.acceptedUnifiedBlocks.begin());
        output.frames.push_back(std::move(result));
    }
    return output;
}

bool RunDecoderToCompletion(const std::filesystem::path& root, const GrayFrames& frames,
    const std::span<const std::byte> expected)
{
    using namespace g16test;
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    auto config = pbapp::MakeUnifiedDecoderConfig(root.wstring(), Region());
    config.visualProfile = pbapp::VisualProfile::UnifiedGray;
    const auto start = runtime.Start(config);
    Check(static_cast<bool>(start), start.message.c_str());
    for (const auto& frame : frames.frames)
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

TEST_CASE("Unified-gray frames recover whole files at 1:1", "[application][unified-gray][workflow]")
{
    const auto root = std::filesystem::temp_directory_path() /
        ("pb-gray-11-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    GrayFrames frames = MakeGrayFrames(root, source, 4, false);
    REQUIRE(frames.chromaLaneErasedFrames == 0);
    REQUIRE(frames.decodedTransportBlocks >= 4 * 8);
    REQUIRE(RunDecoderToCompletion(root, frames, source));
    std::filesystem::remove_all(root);
}

TEST_CASE("Unified-gray frames recover whole files through 4:2:0 chroma subsampling",
    "[application][unified-gray][workflow][chroma420]")
{
    const auto root = std::filesystem::temp_directory_path() /
        ("pb-gray-420-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    const std::vector<std::byte> source = g16test::RawBytes(60000);
    GrayFrames frames = MakeGrayFrames(root, source, 4, true);
    // The channel that erases the SC6-V3 Chroma carrier leaves the gray state
    // lane live on every frame; whole-file recovery still completes.
    REQUIRE(frames.chromaLaneErasedFrames == 0);
    REQUIRE(frames.decodedTransportBlocks >= 4 * 8);
    REQUIRE(RunDecoderToCompletion(root, frames, source));
    std::filesystem::remove_all(root);
}

TEST_CASE("Product SC6-V3 encoding is untouched by the unified-gray identity",
    "[application][unified-gray][identity]")
{
    // The gray option must not leak into the product path: token parsing maps
    // it to its own enum value and the product binding stays on the SC6-V3
    // pair.
    REQUIRE(pbapp::ParseVisualProfileToken("unified-gray") == pbapp::VisualProfile::UnifiedGray);
    REQUIRE(pbapp::ParseVisualProfileToken(L"unified-gray") == pbapp::VisualProfile::UnifiedGray);
    REQUIRE(pbapp::ParseVisualProfileToken("unified") == pbapp::VisualProfile::UnifiedLc4);
    REQUIRE(pbapp::ParseVisualProfileToken("unified-bands") == pbapp::VisualProfile::UnifiedBands);
    REQUIRE(pbapp::FindVisualProfileOption(pbapp::VisualProfile::UnifiedGray) != nullptr);
    REQUIRE(pbapp::IsUnifiedVisualFamily(pbapp::VisualProfile::UnifiedGray));
    REQUIRE(pbapp::IsUnifiedVisualFamily(pbapp::VisualProfile::UnifiedLc4));
    REQUIRE_FALSE(pbprotocol::IsProductSessionVisualProfileId(
        pbprotocol::kGrayStatesExperimentalProfile.visualProfileId));
}
