#include "desktop_levels_test_support.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <sstream>

namespace
{
std::vector<float> Metrics(const std::span<const std::byte> data)
{
    std::vector<float> result(data.size() * 8);
    for (std::size_t bit = 0; bit < result.size(); bit++)
    {
        result[bit] = (std::to_integer<unsigned>(data[bit / 8]) & (1u << (bit % 8))) != 0 ? -1.0f : 1.0f;
    }
    return result;
}
} // namespace

TEST_CASE("DesktopLevels diagnostic generator matches independent Transport LDPC oracle", "[desktop-levels][fec][golden]")
{
    for (const unsigned tile : {2u, 4u})
    {
        const auto record = desktoptest::Record(tile);
        const auto golden = desktoptest::Load("tile" + std::to_string(tile) + "-diagnostic.bin", tile == 2 ? 86688 : 21672);
        std::vector<std::byte> generated(golden.size(), std::byte{99});
        REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, generated));
        REQUIRE(generated == golden);
        auto invalidRecord = record;
        invalidRecord[8] ^= std::byte{1};
        REQUIRE_FALSE(pbdesktoplevels::GenerateDiagnosticData(invalidRecord, generated));
        REQUIRE(generated == golden);
        REQUIRE_FALSE(pbdesktoplevels::GenerateDiagnosticData(record, std::span(generated).first(generated.size() - 1)));
        REQUIRE(generated == golden);

        auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        REQUIRE(created);
        auto channel = std::move(created).Value();
        std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, golden, pixels));
        const auto observation = channel.Decode(desktoptest::View(pixels));
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.comparedCodedBits == (tile == 2 ? 680400 : 162000));
        REQUIRE(observation.evaluation.erroneousCodedBits == 0);
        REQUIRE(observation.evaluation.iterationsTotal == 0);
        REQUIRE(observation.evaluation.iterationsMaximum == 0);
        pbdesktoplevels::ReferenceStatistics statistics;
        REQUIRE(statistics.Add(observation.evaluation, 0, channel.GetMarginHistogram(), observation.modulation.margin.minimum));
        const auto summary = statistics.GetSummary();
        REQUIRE(summary.frames == 1);
        REQUIRE(summary.preFecFailedFrames == 0);
        REQUIRE(summary.postFecFailedFrames == 0);
        REQUIRE(summary.verifiedPhases == 1);
        REQUIRE(summary.margin.minimum == 1);
    }
}

TEST_CASE("DesktopLevels FEC scoring retains failures and never repairs from known truth", "[desktop-levels][fec][errors]")
{
    const auto record = desktoptest::Record(4);
    const auto golden = desktoptest::Load("tile4-diagnostic.bin", 21672);
    auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(created);
    auto channel = std::move(created).Value();
    auto data = golden;
    SECTION("one incorrect coded bit is recovered exactly")
    {
        data[100] ^= std::byte{1};
        const auto result = channel.EvaluateCodewords(record, data, Metrics(data));
        REQUIRE(result.IsVerified());
        REQUIRE(result.erroneousCodedBits == 1);
        REQUIRE(result.comparedCodedBits == 162000);
        REQUIRE(result.falseAcceptedCodewords == 0);
        REQUIRE(result.iterationsMaximum > 0);
    }
    SECTION("tail errors do not dilute coded BER and do fail post FEC")
    {
        data.back() = std::byte{1};
        const auto result = channel.EvaluateCodewords(record, data, Metrics(data));
        REQUIRE(result.evaluated);
        REQUIRE_FALSE(result.IsVerified());
        REQUIRE_FALSE(result.paddingValid);
        REQUIRE(result.erroneousCodedBits == 0);
        REQUIRE(result.comparedCodedBits == 162000);
    }
    SECTION("valid LDPC with a bad transport CRC fails")
    {
        auto information = std::vector<std::byte>(data.begin(), data.begin() + 1350);
        information[100] ^= std::byte{1};
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, std::span(data).first(2025)));
        const auto result = channel.EvaluateCodewords(record, data, Metrics(data));
        REQUIRE_FALSE(result.IsVerified());
        REQUIRE(result.crcFailures == 1);
        REQUIRE(result.fecFailures == 0);
        REQUIRE(result.falseAcceptedCodewords == 0);
    }
    SECTION("wrong but CRC-valid payload is detected as false acceptance")
    {
        const auto block = pbprotocol::ParseTransportBlock(std::span(golden).first(1350));
        REQUIRE(block);
        auto payload = std::vector<std::byte>(block.Value().payload.begin(), block.Value().payload.end());
        payload[0] ^= std::byte{1};
        std::array<std::byte, 1350> information{};
        REQUIRE(pbprotocol::SerializeTransportBlock(block.Value().header, payload, information));
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, std::span(data).first(2025)));
        const auto result = channel.EvaluateCodewords(record, data, Metrics(data));
        REQUIRE(result.evaluated);
        REQUIRE_FALSE(result.IsVerified());
        REQUIRE(result.falseAcceptedCodewords == 1);
        REQUIRE(result.identityFailures == 0);
    }
    SECTION("CRC-valid wrong slot fails identity independently")
    {
        std::copy_n(golden.begin() + 2025, 2025, data.begin());
        const auto result = channel.EvaluateCodewords(record, data, Metrics(data));
        REQUIRE_FALSE(result.IsVerified());
        REQUIRE(result.identityFailures == 1);
        REQUIRE(result.falseAcceptedCodewords == 1);
    }
    SECTION("no confidence is not a truth-recovery path")
    {
        const auto result = channel.EvaluateCodewords(record, data, std::vector<float>(data.size() * 8, 0));
        REQUIRE(result.evaluated);
        REQUIRE_FALSE(result.IsVerified());
        REQUIRE(result.crcFailures + result.fecFailures == 10);
        REQUIRE(result.erroneousCodedBits == 0);
        REQUIRE(result.iterationsTotal == 0); // zero soft decisions pass parity, not Transport/truth
    }
    SECTION("nonfinite and short metric inputs cannot be evaluated")
    {
        auto metrics = Metrics(data);
        metrics.back() = std::numeric_limits<float>::infinity();
        REQUIRE_FALSE(channel.EvaluateCodewords(record, data, metrics).evaluated);
        metrics.back() = 1;
        REQUIRE_FALSE(channel.EvaluateCodewords(record, data, std::span(metrics).first(metrics.size() - 1)).evaluated);
    }
}

TEST_CASE("DesktopLevels raster disturbances correct one bit and reject a wholly erased codeword", "[desktop-levels][fec][raster]")
{
    for (const unsigned tile : {2u, 4u})
    {
        const auto record = desktoptest::Record(tile);
        const auto golden = desktoptest::Load("tile" + std::to_string(tile) + "-diagnostic.bin", tile == 2 ? 86688 : 21672);
        const auto positions = desktoptest::Coordinates(tile);
        auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        REQUIRE(created);
        auto channel = std::move(created).Value();
        for (const bool erasedCodeword : {false, true})
        {
            std::vector<std::byte> pixels(1920 * 1080 * 4);
            REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, golden, pixels));
            std::uint64_t expectedErrors = 1;
            if (erasedCodeword)
            {
                // Black clips every tile in slot zero, hence zero soft input.
                // Coordinates and scattering are independent of the renderer.
                for (std::size_t logical = 0; logical < 2025 * 4; logical++)
                {
                    const auto position = positions[(logical * 65537ULL) % positions.size()];
                    desktoptest::Paint(pixels, position[0], position[1], tile, tile, 0);
                }
                expectedErrors = 0;
                for (std::size_t index = 0; index < 2025; index++)
                {
                    expectedErrors += std::popcount(std::to_integer<unsigned>(golden[index]));
                }
            }
            else
            {
                // Change exactly bit 800 through an adjacent Gray level, not
                // through decoder scratch or an injected post-demod bit array.
                const auto position = positions[(400 * 65537ULL) % positions.size()];
                const unsigned wrongLabel = (std::to_integer<unsigned>(golden[100]) & 3u) ^ 1u;
                constexpr std::array<unsigned, 4> levelsByLabel{32, 96, 224, 160};
                desktoptest::Paint(pixels, position[0], position[1], tile, tile, levelsByLabel[wrongLabel]);
            }
            const auto result = channel.Decode(desktoptest::View(pixels));
            REQUIRE(result.modulation.IsAccepted());
            REQUIRE(result.evaluation.evaluated);
            REQUIRE(result.evaluation.IsVerified() == !erasedCodeword);
            REQUIRE(result.evaluation.paddingValid);
            REQUIRE(result.evaluation.erroneousCodedBits == expectedErrors);
            REQUIRE(result.evaluation.comparedCodedBits == (tile == 2 ? 680400 : 162000));
            REQUIRE(result.evaluation.falseAcceptedCodewords == 0);
            REQUIRE(result.evaluation.identityFailures == 0);
            REQUIRE(result.modulation.unreliableTiles == (erasedCodeword ? 8100u : 0u));
            if (erasedCodeword)
            {
                REQUIRE(result.evaluation.fecFailures + result.evaluation.crcFailures == 1);
            }
            else
            {
                REQUIRE(result.evaluation.iterationsMaximum > 0);
                REQUIRE(result.evaluation.iterationsMaximum <= 48);
            }
            pbdesktoplevels::ReferenceStatistics statistics;
            REQUIRE(statistics.Add(result.evaluation, 0, channel.GetMarginHistogram(), result.modulation.margin.minimum));
            const auto summary = statistics.GetSummary();
            REQUIRE(summary.frames == 1);
            REQUIRE(summary.preFecFailedFrames == 1);
            REQUIRE(summary.postFecFailedFrames == (erasedCodeword ? 1u : 0u));
            REQUIRE(summary.verifiedFrames == (erasedCodeword ? 0u : 1u));
        }
    }
}

TEST_CASE("DesktopLevels fixed adapter and metric denominators are explicit", "[desktop-levels][metrics]")
{
    const std::array<float, 7> metrics{0, 1, -1, 4, -4, 1000, -1000};
    std::array<std::int16_t, 7> adapted{};
    REQUIRE(pbdesktoplevels::AdaptSoftMetrics(metrics, adapted));
    REQUIRE(adapted == std::array<std::int16_t, 7>{0, 4096, -4096, 16384, -16384, 32767, -32767});
    const auto saved = adapted;
    auto invalid = metrics;
    invalid[2] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(pbdesktoplevels::AdaptSoftMetrics(invalid, adapted));
    REQUIRE(adapted == saved);
    REQUIRE_FALSE(pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes - 1));

    pbdesktoplevels::ReferenceStatistics statistics;
    std::ostringstream empty;
    pbdesktoplevels::WriteStatisticsJson(empty, statistics.GetSummary());
    REQUIRE(empty.str().find("\"PreFecBER\":null") != std::string::npos);
    REQUIRE(empty.str().find("\"PostFecFER\":null") != std::string::npos);
    pbdesktoplevels::FrameEvaluation evaluation;
    evaluation.evaluated = evaluation.paddingValid = true;
    evaluation.codewords = 10;
    evaluation.acceptedTransportBlocks = 10;
    evaluation.comparedCodedBits = 162000;
    std::array<std::uint64_t, 4096> histogram{};
    histogram.back() = 86688;
    REQUIRE(statistics.Add(evaluation, 0, histogram, 1));
    evaluation.erroneousCodedBits = 2;
    evaluation.fecFailures = 1;
    REQUIRE_FALSE(statistics.Add(evaluation, 1, histogram, 1));
    REQUIRE(statistics.GetSummary().frames == 1);
    evaluation.acceptedTransportBlocks = 9;
    REQUIRE(statistics.Add(evaluation, 1, histogram, 1));
    const auto summary = statistics.GetSummary();
    REQUIRE(summary.frames == 2);
    REQUIRE(summary.verifiedFrames == 1);
    REQUIRE(summary.preFecFailedFrames == 1);
    REQUIRE(summary.postFecFailedFrames == 1);
    REQUIRE(summary.comparedCodedBits == 324000);
    REQUIRE(summary.erroneousCodedBits == 2);
    REQUIRE(summary.observedPhases == 3);
    REQUIRE(summary.verifiedPhases == 1);
    std::ostringstream json;
    pbdesktoplevels::WriteStatisticsJson(json, summary);
    REQUIRE(json.str().find("\"PreFecFER\":0.5") != std::string::npos);
    REQUIRE(json.str().find("\"PostFecFER\":0.5") != std::string::npos);
    histogram[0] = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(statistics.Add(evaluation, 2, histogram, 0));
    REQUIRE(statistics.GetSummary().frames == 2);
    statistics.Reset();
    REQUIRE(statistics.GetSummary().frames == 0);
}
